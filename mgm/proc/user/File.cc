// ----------------------------------------------------------------------
// File: proc/user/File.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the AGNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "mgm/ProcInterface.hh"
#include "mgm/XrdMgmOfs.hh"
#include "mgm/Access.hh"
#include "mgm/Quota.hh"
#include "common/LayoutId.hh"
/*----------------------------------------------------------------------------*/
#include <math.h>

/*----------------------------------------------------------------------------*/

EOSMGMNAMESPACE_BEGIN

int
ProcCommand::File ()
{
  XrdOucString spath = pOpaque->Get("mgm.path");

  const char* inpath = spath.c_str();
  if (!inpath)
  {
    inpath = "";
  }

  NAMESPACEMAP;
  info = 0;
  if (info)info = 0; // for compiler happyness
  PROC_BOUNCE_ILLEGAL_NAMES;
  PROC_BOUNCE_NOT_ALLOWED;

  spath = path;

  if (!spath.length())
  {
    stdErr = "error: you have to give a path name to call 'file'";
    retc = EINVAL;
  }
  else
  {
    // --------------------------------------------------------------------------
    // drop a replica referenced by filesystem id
    // --------------------------------------------------------------------------
    if (mSubCmd == "drop")
    {
      XrdOucString sfsid = pOpaque->Get("mgm.file.fsid");
      XrdOucString sforce = pOpaque->Get("mgm.file.force");
      bool forceRemove = false;
      if (sforce.length() && (sforce == "1"))
      {
        forceRemove = true;
      }

      unsigned long fsid = (sfsid.length()) ? strtoul(sfsid.c_str(), 0, 10) : 0;

      if (gOFS->_dropstripe(spath.c_str(), *mError, *pVid, fsid, forceRemove))
      {
        stdErr += "error: unable to drop stripe";
        retc = errno;
      }
      else
      {
        stdOut += "success: dropped stripe on fs=";
        stdOut += (int) fsid;
      }
    }


    // --------------------------------------------------------------------------
    // change the number of stripes for files with replica layout
    // --------------------------------------------------------------------------
    if (mSubCmd == "layout")
    {
      XrdOucString stripes = pOpaque->Get("mgm.file.layout.stripes");
      int newstripenumber = 0;
      if (stripes.length()) newstripenumber = atoi(stripes.c_str());
      if (!stripes.length() || ((newstripenumber < (eos::common::LayoutId::kOneStripe + 1)) || (newstripenumber > (eos::common::LayoutId::kSixteenStripe + 1))))
      {
        stdErr = "error: you have to give a valid number of stripes as an argument to call 'file layout'";
        retc = EINVAL;
      }
      else
      {
        // only root can do that
        if (pVid->uid == 0)
        {
          eos::FileMD* fmd = 0;
          if ((spath.beginswith("fid:") || (spath.beginswith("fxid:"))))
          {
            unsigned long long fid = 0;
            if (spath.beginswith("fid:"))
            {
              spath.replace("fid:", "");
              fid = strtoull(spath.c_str(), 0, 10);
            }
            if (spath.beginswith("fxid:"))
            {
              spath.replace("fxid:", "");
              fid = strtoull(spath.c_str(), 0, 16);
            }
            // reference by fid+fsid
            //-------------------------------------------
            gOFS->eosViewRWMutex.LockWrite();
            try
            {
              fmd = gOFS->eosFileService->getFileMD(fid);
            }
            catch (eos::MDException &e)
            {
              errno = e.getErrno();
              stdErr = "error: cannot retrieve file meta data - ";
              stdErr += e.getMessage().str().c_str();
              eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
            }
          }
          else
          {
            // reference by path
            //-------------------------------------------
            gOFS->eosViewRWMutex.LockWrite();
            try
            {
              fmd = gOFS->eosView->getFile(spath.c_str());
            }
            catch (eos::MDException &e)
            {
              errno = e.getErrno();
              stdErr = "error: cannot retrieve file meta data - ";
              stdErr += e.getMessage().str().c_str();
              eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
            }
          }

          if (fmd)
          {
            if ((eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kReplica) || (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kPlain))
            {
              unsigned long newlayout = eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica, eos::common::LayoutId::GetChecksum(fmd->getLayoutId()), newstripenumber, eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()));
              fmd->setLayoutId(newlayout);
              stdOut += "success: setting new stripe number to ";
              stdOut += newstripenumber;
              stdOut += " for path=";
              stdOut += spath;
              // commit new layout
              gOFS->eosView->updateFileStore(fmd);
            }
            else
            {
              retc = EPERM;
              stdErr = "error: you can only change the number of stripes for files with replica layout";
            }
          }
          else
          {
            retc = errno;
          }
          gOFS->eosViewRWMutex.UnLockWrite();
          //-------------------------------------------

        }
        else
        {
          retc = EPERM;
          stdErr = "error: you have to take role 'root' to execute this command";
        }
      }
    }

    // --------------------------------------------------------------------------
    // verify checksum, size for files issuing an asynchronous verification req
    // --------------------------------------------------------------------------
    if (mSubCmd == "verify")
    {
      XrdOucString option = "";
      XrdOucString computechecksum = pOpaque->Get("mgm.file.compute.checksum");
      XrdOucString commitchecksum = pOpaque->Get("mgm.file.commit.checksum");
      XrdOucString commitsize = pOpaque->Get("mgm.file.commit.size");
      XrdOucString commitfmd = pOpaque->Get("mgm.file.commit.fmd");
      XrdOucString verifyrate = pOpaque->Get("mgm.file.verify.rate");

      if (computechecksum == "1")
      {
        option += "&mgm.verify.compute.checksum=1";
      }

      if (commitchecksum == "1")
      {
        option += "&mgm.verify.commit.checksum=1";
      }

      if (commitsize == "1")
      {
        option += "&mgm.verify.commit.size=1";
      }

      if (commitfmd == "1")
      {
        option += "&mgm.verify.commit.fmd=1";
      }

      if (verifyrate.length())
      {
        option += "&mgm.verify.rate=";
        option += verifyrate;
      }

      XrdOucString fsidfilter = pOpaque->Get("mgm.file.verify.filterid");
      int acceptfsid = 0;
      if (fsidfilter.length())
      {
        acceptfsid = atoi(pOpaque->Get("mgm.file.verify.filterid"));
      }

      // only root can do that
      if (pVid->uid == 0)
      {
        eos::FileMD* fmd = 0;
        if ((spath.beginswith("fid:") || (spath.beginswith("fxid:"))))
        {
          unsigned long long fid = 0;
          if (spath.beginswith("fid:"))
          {
            spath.replace("fid:", "");
            fid = strtoull(spath.c_str(), 0, 10);
          }
          if (spath.beginswith("fxid:"))
          {
            spath.replace("fxid:", "");
            fid = strtoull(spath.c_str(), 0, 16);
          }
          // reference by fid+fsid
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try
          {
            fmd = gOFS->eosFileService->getFileMD(fid);
            std::string fullpath = gOFS->eosView->getUri(fmd);
            spath = fullpath.c_str();
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
          }
        }
        else
        {
          // reference by path
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try
          {
            fmd = gOFS->eosView->getFile(spath.c_str());
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
          }
        }

        if (fmd)
        {
          // copy out the locations vector
          eos::FileMD::LocationVector locations;
          eos::FileMD::LocationVector::const_iterator it;
          for (it = fmd->locationsBegin(); it != fmd->locationsEnd(); ++it)
          {
            locations.push_back(*it);
          }

          gOFS->eosViewRWMutex.UnLockRead();

          retc = 0;
          bool acceptfound = false;

          for (it = locations.begin(); it != locations.end(); ++it)
          {
            if (acceptfsid && (acceptfsid != (int) *it))
            {
              continue;
            }
            if (acceptfsid)
              acceptfound = true;

            int lretc = gOFS->_verifystripe(spath.c_str(), *mError, vid, (unsigned long) *it, option);
            if (!lretc)
            {
              stdOut += "success: sending verify to fsid= ";
              stdOut += (int) *it;
              stdOut += " for path=";
              stdOut += spath;
              stdOut += "\n";
            }
            else
            {
              retc = errno;
            }
          }

          // we want to be able to force the registration and verification of a not registered replica
          if (acceptfsid && (!acceptfound))
          {
            int lretc = gOFS->_verifystripe(spath.c_str(), *mError, vid, (unsigned long) acceptfsid, option);
            if (!lretc)
            {
              stdOut += "success: sending forced verify to fsid= ";
              stdOut += acceptfsid;
              stdOut += " for path=";
              stdOut += spath;
              stdOut += "\n";
            }
            else
            {
              retc = errno;
            }
          }
        }
        else
        {
          gOFS->eosViewRWMutex.UnLockRead();
        }

        //-------------------------------------------

      }
      else
      {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' to execute this command";
      }
    }

    // --------------------------------------------------------------------------
    // move a replica/stripe from source fs to target fs
    // --------------------------------------------------------------------------
    if (mSubCmd == "move")
    {
      XrdOucString sfsidsource = pOpaque->Get("mgm.file.sourcefsid");
      unsigned long sourcefsid = (sfsidsource.length()) ? strtoul(sfsidsource.c_str(), 0, 10) : 0;
      XrdOucString sfsidtarget = pOpaque->Get("mgm.file.targetfsid");
      unsigned long targetfsid = (sfsidsource.length()) ? strtoul(sfsidtarget.c_str(), 0, 10) : 0;

      if (gOFS->_movestripe(spath.c_str(), *mError, *pVid, sourcefsid, targetfsid))
      {
        stdErr += "error: unable to move stripe";
        retc = errno;
      }
      else
      {
        stdOut += "success: scheduled move from source fs=";
        stdOut += sfsidsource;
        stdOut += " => target fs=";
        stdOut += sfsidtarget;
      }
    }

    // --------------------------------------------------------------------------
    // replicate a replica/stripe from source fs to target fs
    // --------------------------------------------------------------------------
    if (mSubCmd == "replicate")
    {
      XrdOucString sfsidsource = pOpaque->Get("mgm.file.sourcefsid");
      unsigned long sourcefsid = (sfsidsource.length()) ? strtoul(sfsidsource.c_str(), 0, 10) : 0;
      XrdOucString sfsidtarget = pOpaque->Get("mgm.file.targetfsid");
      unsigned long targetfsid = (sfsidtarget.length()) ? strtoul(sfsidtarget.c_str(), 0, 10) : 0;

      if (gOFS->_copystripe(spath.c_str(), *mError, *pVid, sourcefsid, targetfsid))
      {
        stdErr += "error: unable to replicate stripe";
        retc = errno;
      }
      else
      {
        stdOut += "success: scheduled replication from source fs=";
        stdOut += sfsidsource;
        stdOut += " => target fs=";
        stdOut += sfsidtarget;
      }
    }

    // --------------------------------------------------------------------------
    // rename a file or directory from source to target path
    // --------------------------------------------------------------------------
    if (mSubCmd == "rename")
    {
      XrdOucString source = pOpaque->Get("mgm.file.source");
      XrdOucString target = pOpaque->Get("mgm.file.target");

      if (gOFS->rename(source.c_str(), target.c_str(), *mError, *pVid, 0, 0))
      {
        stdErr += "error: unable to rename";
        retc = errno;
      }
      else
      {
        stdOut += "success: renamed '";
        stdOut += source.c_str();
        stdOut += "' to '";
        stdOut += target.c_str();
        stdOut += "'";
      }
    }

    // --------------------------------------------------------------------------Ï
    // fix the current state of the file layout by removing/repairing or adding
    // replica/stripes
    // --------------------------------------------------------------------------
    if (mSubCmd == "adjustreplica")
    {
      // only root can do that
      if (pVid->uid == 0)
      {
        eos::FileMD* fmd = 0;
	bool nodrop = false;

	if (pOpaque->Get("mgm.file.option") && (!strcmp(pOpaque->Get("mgm.file.option"),"nodrop"))) 
	{
	  nodrop = true;
	}

        // this flag indicates that the replicate command should queue this transfers on the head of the FST transfer lists
        XrdOucString sexpressflag = (pOpaque->Get("mgm.file.express"));
        bool expressflag = false;
        if (sexpressflag == "1")
          expressflag = 1;

        XrdOucString creationspace = pOpaque->Get("mgm.file.desiredspace");
        int icreationsubgroup = -1;

        if (pOpaque->Get("mgm.file.desiredsubgroup"))
        {
          icreationsubgroup = atoi(pOpaque->Get("mgm.file.desiredsubgroup"));
        }

        if ((spath.beginswith("fid:") || (spath.beginswith("fxid:"))))
        {
          unsigned long long fid = 0;
          if (spath.beginswith("fid:"))
          {
            spath.replace("fid:", "");
            fid = strtoull(spath.c_str(), 0, 10);
          }
          if (spath.beginswith("fxid:"))
          {
            spath.replace("fxid:", "");
            fid = strtoull(spath.c_str(), 0, 16);
          }

          // reference by fid+fsid
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try
          {
            fmd = gOFS->eosFileService->getFileMD(fid);
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
          }
        }
        else
        {
          // reference by path
          //-------------------------------------------
          gOFS->eosViewRWMutex.LockRead();
          try
          {
            fmd = gOFS->eosView->getFile(spath.c_str());
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
          }
        }

        XrdOucString space = "default";
        XrdOucString refspace = "";
        unsigned int forcedsubgroup = 0;


        if (fmd)
        {
          unsigned long long fid = fmd->getId();
          eos::FileMD fmdCopy(*fmd);
          fmd = &fmdCopy;

          gOFS->eosViewRWMutex.UnLockRead();
          //-------------------------------------------

          // check if that is a replica layout at all
          if (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) == eos::common::LayoutId::kReplica)
          {
            // check the configured and available replicas

            XrdOucString sizestring;

            eos::FileMD::LocationVector::const_iterator lociter;
            int nreplayout = eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()) + 1;
            int nrep = (int) fmd->getNumLocation();
            int nreponline = 0;
            int ngroupmix = 0;
            for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
            {
              // ignore filesystem id 0
              if (!(*lociter))
              {
                eos_err("fsid 0 found fid=%lld", fmd->getId());
                continue;
              }

              eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

              FileSystem* filesystem = 0;
              if (FsView::gFsView.mIdView.count((int) *lociter))
                filesystem = FsView::gFsView.mIdView[(int) *lociter];
              if (filesystem)
              {
                eos::common::FileSystem::fs_snapshot_t snapshot;
                filesystem->SnapShotFileSystem(snapshot, true);

                // remember the spacename
                space = snapshot.mSpace.c_str();

                if (!refspace.length())
                {
                  refspace = space;
                }
                else
                {
                  if (space != refspace)
                  {
                    ngroupmix++;
                  }
                }

                forcedsubgroup = snapshot.mGroupIndex;

                if (
                    (snapshot.mConfigStatus > eos::common::FileSystem::kDrain) &&
                    (snapshot.mStatus == eos::common::FileSystem::kBooted)
                    )
                {
                  // this is a good accessible one
                  nreponline++;
                }
              }
            }

            eos_debug("path=%s nrep=%lu nrep-layout=%lu nrep-online=%lu", spath.c_str(), nrep, nreplayout, nreponline);

            if (nreplayout > nreponline)
            {
              // set the desired space & subgroup if provided
              if (creationspace.length())
              {
                space = creationspace;
              }

              if (icreationsubgroup != -1)
              {
                forcedsubgroup = icreationsubgroup;
              }

              // if the space is explicitly set, we don't force into a particular subgroup
              if (creationspace.length())
              {
                forcedsubgroup = -1;
              }

              // we don't have enough replica's online, we trigger asynchronous replication
              int nnewreplicas = nreplayout - nreponline; // we have to create that much new replica

              eos_debug("forcedsubgroup=%d icreationsubgroup=%d", forcedsubgroup, icreationsubgroup);

              // get the location where we can read that file
              SpaceQuota* quotaspace = Quota::GetSpaceQuota(space.c_str(), true);
              eos_debug("creating %d new replicas space=%s subgroup=%d", nnewreplicas, space.c_str(), forcedsubgroup);

              if (!quotaspace)
              {
                stdErr = "error: create new replicas => cannot get space: ";
                stdErr += space;
                stdErr += "\n";
                errno = ENOSPC;
              }
              else
              {
                unsigned long fsIndex; // this defines the fs to use in the selectefs vector
                std::vector<unsigned int> selectedfs;
                std::vector<unsigned int> unavailfs;
                // fill the existing locations
                for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
                {
                  selectedfs.push_back(*lociter);
                }

                if (!(errno = quotaspace->FileAccess(*pVid, (unsigned long) 0, space.c_str(), (unsigned long) fmd->getLayoutId(), selectedfs, fsIndex, false, (long long unsigned) 0, unavailfs)))
                {
                  // this is now our source filesystem
                  unsigned int sourcefsid = selectedfs[fsIndex];
                  // now the just need to ask for <n> targets
                  int layoutId = eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica, eos::common::LayoutId::kNone, nnewreplicas);

                  // we don't know the container tag here, but we don't really care since we are scheduled as root
                  if (!(errno = quotaspace->FilePlacement(spath.c_str(), *pVid, 0, layoutId, selectedfs, selectedfs, SFS_O_TRUNC, forcedsubgroup, fmd->getSize())))
                  {
                    // yes we got a new replication vector
                    for (unsigned int i = 0; i < selectedfs.size(); i++)
                    {
                      //                      stdOut += "info: replication := "; stdOut += (int) sourcefsid; stdOut += " => "; stdOut += (int)selectedfs[i]; stdOut += "\n";
                      // add replication here
                      if (gOFS->_replicatestripe(fmd, spath.c_str(), *mError, *pVid, sourcefsid, selectedfs[i], false, expressflag))
                      {
                        stdErr += "error: unable to replicate stripe ";
                        stdErr += (int) sourcefsid;
                        stdErr += " => ";
                        stdErr += (int) selectedfs[i];
                        stdErr += "\n";
                        retc = errno;
                      }
                      else
                      {
                        stdOut += "success: scheduled replication from source fs=";
                        stdOut += (int) sourcefsid;
                        stdOut += " => target fs=";
                        stdOut += (int) selectedfs[i];
                        stdOut += "\n";
                      }
                    }
                  }
                  else
                  {
                    stdErr = "error: create new replicas => cannot place replicas: ";
                    stdErr += spath;
                    stdErr += "\n";
                    retc = ENOSPC;
                  }
                }
                else
                {
                  stdErr = "error: create new replicas => no source available: ";
                  stdErr += spath;
                  stdErr += "\n";
                  retc = ENONET;
                }
              }
            }
            else
            {
              // we do this only if we didn't create replicas in the if section before, otherwise we remove replicas which have used before for new replications

              // this is magic code to adjust the number of replicas to the desired policy ;-)
              if ( (nreplayout < nrep) && (!nodrop) )
              {
                std::vector<unsigned long> fsid2delete;
                unsigned int n2delete = nrep - nreplayout;

                eos::FileMD::LocationVector locvector;
                // we build three views to sort the order of dropping

                std::multimap <int /*configstate*/, int /*fsid*/> statemap;
                std::multimap <std::string /*schedgroup*/, int /*fsid*/> groupmap;
                std::multimap <std::string /*space*/, int /*fsid*/> spacemap;

                // we have too many replica's online, we drop (nrepoonline-nreplayout) replicas starting with the lowest configuration state

                eos_debug("trying to drop %d replicas space=%s subgroup=%d", n2delete, creationspace.c_str(), icreationsubgroup);

                // fill the views
                for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
                {
                  // ignore filesystem id 0
                  if (!(*lociter))
                  {
                    eos_err("fsid 0 found fid=%lld", fmd->getId());
                    continue;
                  }

                  eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);

                  FileSystem* filesystem = 0;
                  if (FsView::gFsView.mIdView.count((int) *lociter))
                    filesystem = FsView::gFsView.mIdView[(int) *lociter];

                  eos::common::FileSystem::fs_snapshot_t fs;

                  if (filesystem && filesystem->SnapShotFileSystem(fs, true))
                  {
                    unsigned int fsid = filesystem->GetId();
                    statemap.insert(std::pair<int, int>(fs.mConfigStatus, fsid));
                    groupmap.insert(std::pair<std::string, int>(fs.mGroup, fsid));
                    spacemap.insert(std::pair<std::string, int>(fs.mSpace, fsid));
                  }
                }


                if (!creationspace.length())
                {
                  // there is no requirement to keep a certain space
                  std::multimap <int, int>::const_iterator sit;
                  for (sit = statemap.begin(); sit != statemap.end(); ++sit)
                  {
                    fsid2delete.push_back(sit->second);
                    // we add to the deletion vector until we have found enough replicas
                    if (fsid2delete.size() == n2delete)
                      break;
                  }
                }
                else
                {
                  if (!icreationsubgroup)
                  {
                    // we have only a space requirement no subgroup required
                    std::multimap <std::string, int>::const_iterator sit;
                    std::multimap <int, int> limitedstatemap;

                    std::string cspace = creationspace.c_str();

                    for (sit = spacemap.begin(); sit != spacemap.end(); ++sit)
                    {

                      // match the space name
                      if (sit->first == cspace)
                      {
                        continue;
                      }

                      // we default to the highest state for safety reasons
                      int state = eos::common::FileSystem::kRW;

                      std::multimap <int, int>::const_iterator stateit;

                      // get the state for each fsid matching
                      for (stateit = statemap.begin(); stateit != statemap.end(); stateit++)
                      {
                        if (stateit->second == sit->second)
                        {
                          state = stateit->first;
                          break;
                        }
                      }

                      // fill the map containing only the candidates
                      limitedstatemap.insert(std::pair<int, int>(state, sit->second));
                    }

                    std::multimap <int, int>::const_iterator lit;

                    for (lit = limitedstatemap.begin(); lit != limitedstatemap.end(); ++lit)
                    {
                      fsid2delete.push_back(lit->second);
                      if (fsid2delete.size() == n2delete)
                        break;
                    }
                  }
                  else
                  {
                    // we have a clear requirement on space/subgroup
                    std::multimap <std::string, int>::const_iterator sit;
                    std::multimap <int, int> limitedstatemap;

                    std::string cspace = creationspace.c_str();
                    cspace += ".";
                    cspace += icreationsubgroup;

                    for (sit = groupmap.begin(); sit != groupmap.end(); ++sit)
                    {

                      // match the space name
                      if (sit->first == cspace)
                      {
                        continue;
                      }


                      // we default to the highest state for safety reasons
                      int state = eos::common::FileSystem::kRW;

                      std::multimap <int, int>::const_iterator stateit;

                      // get the state for each fsid matching
                      for (stateit = statemap.begin(); stateit != statemap.end(); stateit++)
                      {
                        if (stateit->second == sit->second)
                        {
                          state = stateit->first;
                          break;
                        }
                      }

                      // fill the map containing only the candidates
                      limitedstatemap.insert(std::pair<int, int>(state, sit->second));
                    }

                    std::multimap <int, int>::const_iterator lit;

                    for (lit = limitedstatemap.begin(); lit != limitedstatemap.end(); ++lit)
                    {
                      fsid2delete.push_back(lit->second);
                      if (fsid2delete.size() == n2delete)
                        break;
                    }
                  }
                }

                if (fsid2delete.size() != n2delete)
                {
                  // add a warning that something does not work as requested ....
                  stdErr = "warning: cannot adjust replicas according to your requirement: space=";
                  stdErr += creationspace;
                  stdErr += " subgroup=";
                  stdErr += icreationsubgroup;
                  stdErr += "\n";
                }

                for (unsigned int i = 0; i < fsid2delete.size(); i++)
                {
                  if (fmd->hasLocation(fsid2delete[i]))
                  {
                    //-------------------------------------------
                    eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
                    try
                    {
                      // we have to get again the original file meta data
                      fmd = gOFS->eosFileService->getFileMD(fid);
                      fmd->unlinkLocation(fsid2delete[i]);
                      gOFS->eosView->updateFileStore(fmd);
                      eos_debug("removing location %u", fsid2delete[i]);
                      stdOut += "success: dropping replica on fs=";
                      stdOut += (int) fsid2delete[i];
                      stdOut += "\n";
                    }
                    catch (eos::MDException &e)
                    {
                      errno = e.getErrno();
                      stdErr = "error: drop excess replicas => cannot unlink location - ";
                      stdErr += e.getMessage().str().c_str();
                      stdErr += "\n";
                      eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
                    }
                  }
                }
              }
            }
          }
        }
        else
        {
          gOFS->eosViewRWMutex.UnLockRead();
        }
      }
      else
      {
        retc = EPERM;
        stdErr = "error: you have to take role 'root' to execute this command";
      }
    }

    // --------------------------------------------------------------------------
    // return meta data for a particular file
    // --------------------------------------------------------------------------
    if (mSubCmd == "getmdlocation")
    {
      gOFS->MgmStats.Add("GetMdLocation", pVid->uid, pVid->gid, 1);
      // this returns the access urls to query local metadata information
      XrdOucString spath = pOpaque->Get("mgm.path");

      const char* inpath = spath.c_str();

      NAMESPACEMAP;
      info = 0;
      if (info)info = 0; // for compiler happyness
      PROC_BOUNCE_ILLEGAL_NAMES;
      PROC_BOUNCE_NOT_ALLOWED;

      spath = path;

      if (!spath.length())
      {
        stdErr = "error: you have to give a path name to call 'fileinfo'";
        retc = EINVAL;
      }
      else
      {
        eos::FileMD* fmd = 0;

        //-------------------------------------------
        eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
        try
        {
          fmd = gOFS->eosView->getFile(spath.c_str());
        }
        catch (eos::MDException &e)
        {
          errno = e.getErrno();
          stdErr = "error: cannot retrieve file meta data - ";
          stdErr += e.getMessage().str().c_str();
          eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
        }

        if (!fmd)
        {
          retc = errno;
        }
        else
        {
          eos::FileMD fmdCopy(*fmd);
          fmd = &fmdCopy;

          XrdOucString sizestring;

          eos::FileMD::LocationVector::const_iterator lociter;
          int i = 0;
          stdOut += "&";
          stdOut += "mgm.nrep=";
          stdOut += (int) fmd->getNumLocation();
          stdOut += "&";
          stdOut += "mgm.checksumtype=";
          stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId());
          stdOut += "&";
          stdOut += "mgm.size=";
          stdOut += eos::common::StringConversion::GetSizeString(sizestring, (unsigned long long) fmd->getSize());
          stdOut += "&";
          stdOut += "mgm.checksum=";
          size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());
          for (unsigned int i = 0; i < SHA_DIGEST_LENGTH; i++)
          {
            char hb[3];
            sprintf(hb, "%02x", (i < cxlen) ? ((unsigned char) (fmd->getChecksum().getDataPtr()[i])) : 0);
            stdOut += hb;
          }
          stdOut += "&";
          stdOut += "mgm.stripes=";
          stdOut += (int) (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()) + 1);
          stdOut += "&";

          for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
          {
            // ignore filesystem id 0
            if (!(*lociter))
            {
              eos_err("fsid 0 found fid=%lld", fmd->getId());
              continue;
            }
            eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
            eos::common::FileSystem* filesystem = 0;
            if (FsView::gFsView.mIdView.count(*lociter))
            {
              filesystem = FsView::gFsView.mIdView[*lociter];
            }
            if (filesystem)
            {
              XrdOucString host;
              XrdOucString fullpath = "";
              std::string hostport = filesystem->GetString("hostport");
              stdOut += "mgm.replica.url";
              stdOut += i;
              stdOut += "=";
              stdOut += hostport.c_str();
              stdOut += "&";
              XrdOucString hexstring = "";
              eos::common::FileId::Fid2Hex(fmd->getId(), hexstring);
              stdOut += "mgm.fid";
              stdOut += i;
              stdOut += "=";
              stdOut += hexstring;
              stdOut += "&";
              stdOut += "mgm.fsid";
              stdOut += i;
              stdOut += "=";
              stdOut += (int) *lociter;
              stdOut += "&";
              stdOut += "mgm.fsbootstat";
              stdOut += i;
              stdOut += "=";
              stdOut += filesystem->GetString("stat.boot").c_str();
              stdOut += "&";
              stdOut += "mgm.fstpath";
              stdOut += i;
              stdOut += "=";
              eos::common::FileId::FidPrefix2FullPath(hexstring.c_str(), filesystem->GetPath().c_str(), fullpath);
              stdOut += fullpath;
              stdOut += "&";
            }
            else
            {
              stdOut += "NA&";
            }
            i++;
          }
        }
      }
    }
  }
  return SFS_OK;
}

EOSMGMNAMESPACE_END