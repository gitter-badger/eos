// ----------------------------------------------------------------------
// File: proc/user/Find.cc
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
#include "common/LayoutId.hh"
/*----------------------------------------------------------------------------*/

#ifdef __APPLE__
#define pow10( x ) pow( (float)10, (int)(x) )
#define ENONET 64
#endif

EOSMGMNAMESPACE_BEGIN

int
ProcCommand::Find ()
{
  mDoSort = true;

  XrdOucString spath = pOpaque->Get("mgm.path");
  XrdOucString option = pOpaque->Get("mgm.option");
  XrdOucString attribute = pOpaque->Get("mgm.find.attribute");
  XrdOucString olderthan = pOpaque->Get("mgm.find.olderthan");
  XrdOucString youngerthan = pOpaque->Get("mgm.find.youngerthan");

  XrdOucString key = attribute;
  XrdOucString val = attribute;
  XrdOucString printkey = pOpaque->Get("mgm.find.printkey");

  const char* inpath = spath.c_str();
  bool deepquery = false;
  static XrdSysMutex deepQueryMutex;
  static std::map<std::string, std::set<std::string> > * globalfound = 0;
  NAMESPACEMAP;
  info = 0;
  if (info)info = 0; // for compiler happyness
  PROC_BOUNCE_ILLEGAL_NAMES;
  PROC_BOUNCE_NOT_ALLOWED;

  spath = path;

  if (!OpenTemporaryOutputFiles())
  {
    stdErr += "error: cannot write find result files on MGM\n";
    retc = EIO;
    return SFS_OK;
  }

  eos::common::Path cPath(spath.c_str());
  if (cPath.GetSubPathSize() < 5)
  {
    if ((((option.find("d")) != STR_NPOS) && ((option.find("f")) == STR_NPOS)))
    {
      // directory queries are fine even for the complete namespace
      deepquery = false;
    }
    else
    {
      // deep queries are serialized by a mutex and use a single the output hashmap !
      deepquery = true;
    }
  }

  // this hash is used to calculate the balance of the found files over the filesystems involved
  google::dense_hash_map<unsigned long, unsigned long long> filesystembalance;
  google::dense_hash_map<std::string, unsigned long long> spacebalance;
  google::dense_hash_map<std::string, unsigned long long> schedulinggroupbalance;
  google::dense_hash_map<int, unsigned long long> sizedistribution;
  google::dense_hash_map<int, unsigned long long> sizedistributionn;

  filesystembalance.set_empty_key(0);
  spacebalance.set_empty_key("");
  schedulinggroupbalance.set_empty_key("");
  sizedistribution.set_empty_key(-1);
  sizedistributionn.set_empty_key(-1);

  bool calcbalance = false;
  bool findzero = false;
  bool findgroupmix = false;
  bool printsize = false;
  bool printfid = false;
  bool printfs = false;
  bool printchecksum = false;
  bool printctime = false;
  bool printmtime = false;
  bool printrep = false;
  bool selectrepdiff = false;
  bool selectonehour = false;
  bool printunlink = false;
  bool printcounter = false;
  bool printchildcount = true;
  bool printhosts = false;
  bool printpartition = false;

  time_t selectoldertime = 0;
  time_t selectyoungertime = 0;

  if (olderthan.c_str())
  {
    selectoldertime = (time_t) strtoull(olderthan.c_str(), 0, 10);
  }

  if (youngerthan.c_str())
  {
    selectyoungertime = (time_t) strtoull(youngerthan.c_str(), 0, 10);
  }

  if (option.find("b") != STR_NPOS)
  {
    calcbalance = true;
  }

  if (option.find("0") != STR_NPOS)
  {
    findzero = true;
  }

  if (option.find("G") != STR_NPOS)
  {
    findgroupmix = true;
  }

  if (option.find("S") != STR_NPOS)
  {
    printsize = true;
  }

  if (option.find("F") != STR_NPOS)
  {
    printfid = true;
  }

  if (option.find("L") != STR_NPOS)
  {
    printfs = true;
  }

  if (option.find("X") != STR_NPOS)
  {
    printchecksum = true;
  }

  if (option.find("C") != STR_NPOS)
  {
    printctime = true;
  }

  if (option.find("M") != STR_NPOS)
  {
    printmtime = true;
  }

  if (option.find("R") != STR_NPOS)
  {
    printrep = true;
  }

  if (option.find("U") != STR_NPOS)
  {
    printunlink = true;
  }

  if (option.find("D") != STR_NPOS)
  {
    selectrepdiff = true;
  }

  if (option.find("1") != STR_NPOS)
  {
    selectonehour = true;
  }

  if (option.find("Z") != STR_NPOS)
  {
    printcounter = true;
  }

  if (option.find("l") != STR_NPOS)
  {
    printchildcount = true;
  }

  if (option.find("H") != STR_NPOS)
  {
    printhosts = true;
  }

  if (option.find("P") != STR_NPOS)
  {
    printpartition = true;
  }

  if (attribute.length())
  {
    key.erase(attribute.find("="));
    val.erase(0, attribute.find("=") + 1);
  }

  if (!spath.length())
  {
    fprintf(fstderr, "error: you have to give a path name to call 'find'");
    retc = EINVAL;
  }
  else
  {
    std::map<std::string, std::set<std::string> > * found = 0;
    if (deepquery)
    {
      // we use a single once allocated map for deep searches to store the results to avoid memory explosion
      deepQueryMutex.Lock();

      if (!globalfound)
      {
        globalfound = new std::map<std::string, std::set<std::string> >;
      }
      found = globalfound;
    }
    else
    {
      found = new std::map<std::string, std::set<std::string> >;
    }
    std::map<std::string, std::set<std::string> >::const_iterator foundit;
    std::set<std::string>::const_iterator fileit;
    bool nofiles = false;

    if (((option.find("d")) != STR_NPOS) && ((option.find("f")) == STR_NPOS))
    {
      nofiles = true;
    }

    if (gOFS->_find(spath.c_str(), *mError, stdErr, *pVid, (*found), key.c_str(), val.c_str(), nofiles))
    {
      fprintf(fstderr, "%s", stdErr.c_str());
      fprintf(fstderr, "error: unable to run find in directory");
      retc = errno;
    }

    int cnt = 0;
    unsigned long long filecounter = 0;
    unsigned long long dircounter = 0;

    if (((option.find("f")) != STR_NPOS) || ((option.find("d")) == STR_NPOS))
    {
      for (foundit = (*found).begin(); foundit != (*found).end(); foundit++)
      {

        if ((option.find("d")) == STR_NPOS)
        {
          if (option.find("f") == STR_NPOS)
          {
            if (!printcounter) fprintf(fstdout, "%s\n", foundit->first.c_str());
            dircounter++;
          }
        }

        for (fileit = foundit->second.begin(); fileit != foundit->second.end(); fileit++)
        {
          cnt++;
          std::string fspath = foundit->first;
          fspath += *fileit;
          if (!calcbalance)
          {
            if (findgroupmix || findzero || printsize || printfid || printchecksum || printctime || printmtime || printrep || printunlink || printhosts || printpartition || selectrepdiff || selectonehour || selectoldertime || selectyoungertime)
            {
              //-------------------------------------------

              gOFS->eosViewRWMutex.LockRead();
              eos::FileMD* fmd = 0;
              try
              {
                bool selected = true;

                unsigned long long filesize = 0;
                fmd = gOFS->eosView->getFile(fspath.c_str());
                eos::FileMD fmdCopy(*fmd);
                fmd = &fmdCopy;
                gOFS->eosViewRWMutex.UnLockRead();
                //-------------------------------------------

                if (selectonehour)
                {
                  eos::FileMD::ctime_t mtime;
                  fmd->getMTime(mtime);
                  if (mtime.tv_sec > (time(NULL) - 3600))
                  {
                    selected = false;
                  }
                }

                if (selectoldertime)
                {
                  eos::FileMD::ctime_t mtime;
                  fmd->getMTime(mtime);
                  if (mtime.tv_sec > selectoldertime)
                  {
                    selected = false;
                  }
                }

                if (selectyoungertime)
                {
                  eos::FileMD::ctime_t mtime;
                  fmd->getMTime(mtime);
                  if (mtime.tv_sec < selectyoungertime)
                  {
                    selected = false;
                  }
                }

                if (selected && (findzero || findgroupmix))
                {
                  if (findzero)
                  {
                    if (!(filesize = fmd->getSize()))
                    {
                      if (!printcounter) fprintf(fstdout, "%s\n", fspath.c_str());
                    }
                  }

                  if (selected && findgroupmix)
                  {
                    // find files which have replicas on mixed scheduling groups
                    eos::FileMD::LocationVector::const_iterator lociter;
                    XrdOucString sGroupRef = "";
                    XrdOucString sGroup = "";
                    bool mixed = false;
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
                        sGroup = filesystem->GetString("schedgroup").c_str();
                      }
                      else
                      {
                        sGroup = "none";
                      }

                      if (sGroupRef.length())
                      {
                        if (sGroup != sGroupRef)
                        {
                          mixed = true;
                          break;
                        }
                      }
                      else
                      {
                        sGroupRef = sGroup;
                      }
                    }
                    if (mixed)
                    {
                      if (!printcounter)fprintf(fstdout, "%s\n", fspath.c_str());
                    }
                  }
                }
                else
                {
                  if (selected && (selectonehour || selectoldertime || selectyoungertime || printsize || printfid || printchecksum || printfs || printctime || printmtime || printrep || printunlink || printhosts || printpartition || selectrepdiff))
                  {
                    XrdOucString sizestring;
                    bool printed = true;
                    if (selectrepdiff)
                    {
                      if (fmd->getNumLocation() != (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()) + 1))
                      {
                        printed = true;
                      }
                      else
                      {
                        printed = false;
                      }
                    }

                    if (printed)
                    {
                      if (!printcounter)fprintf(fstdout, "path=%s", fspath.c_str());

                      if (printsize)
                      {
                        if (!printcounter)fprintf(fstdout, " size=%llu", (unsigned long long) fmd->getSize());
                      }
                      if (printfid)
                      {
                        if (!printcounter)fprintf(fstdout, " fid=%llu", (unsigned long long) fmd->getId());
                      }
                      if (printfs)
                      {
                        if (!printcounter)fprintf(fstdout, " fsid=");
                        eos::FileMD::LocationVector::const_iterator lociter;
                        for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
                        {
                          if (lociter != fmd->locationsBegin())
                          {
                            if (!printcounter)fprintf(fstdout, ",");
                          }
                          if (!printcounter)fprintf(fstdout, "%d", (int) *lociter);
                        }
                      }

                      if ( (printpartition) && (!printcounter))
                      {
                        fprintf(fstdout, " partition=");
                        std::set<std::string> fsPartition;
                        eos::FileMD::LocationVector::const_iterator lociter;
                        for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
                        {
                          // get host name for fs id
                          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                          eos::common::FileSystem* filesystem = 0;
                          if (FsView::gFsView.mIdView.count(*lociter))
                          {
                            filesystem = FsView::gFsView.mIdView[*lociter];
                          }

                          if (filesystem)
                          {
                            eos::common::FileSystem::fs_snapshot_t fs;
                            if (filesystem->SnapShotFileSystem(fs, true))
                            {
			      std::string partition = fs.mHost;
			      partition += ":";
			      partition += fs.mPath;
                              fsPartition.insert(partition);
                            }
                          }
                        }
                        
                        for (auto partitionit = fsPartition.begin(); partitionit != fsPartition.end(); partitionit++)
                        {
                          if (partitionit != fsPartition.begin())
                          {
                            fprintf(fstdout, ",");
                          }
                          fprintf(fstdout, "%s", partitionit->c_str());
                        }
                      }

                      if ( (printhosts) && (!printcounter))
                      {
                        fprintf(fstdout, " hosts=");
                        std::set<std::string> fsHosts;
                        eos::FileMD::LocationVector::const_iterator lociter;
                        for (lociter = fmd->locationsBegin(); lociter != fmd->locationsEnd(); ++lociter)
                        {
                          // get host name for fs id
                          eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                          eos::common::FileSystem* filesystem = 0;
                          if (FsView::gFsView.mIdView.count(*lociter))
                          {
                            filesystem = FsView::gFsView.mIdView[*lociter];
                          }

                          if (filesystem)
                          {
                            eos::common::FileSystem::fs_snapshot_t fs;
                            if (filesystem->SnapShotFileSystem(fs, true))
                            {
                              fsHosts.insert(fs.mHost);
                            }
                          }
                        }
                        
                        for (auto hostit = fsHosts.begin(); hostit != fsHosts.end(); hostit++)
                        {
                          if (hostit != fsHosts.begin())
                          {
                            fprintf(fstdout, ",");
                          }
                          fprintf(fstdout, "%s", hostit->c_str());
                        }
                      }

                      if (printchecksum)
                      {
                        if (!printcounter)fprintf(fstdout, " checksum=");
                        for (unsigned int i = 0; i < eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId()); i++)
                        {
                          if (!printcounter)fprintf(fstdout, "%02x", (unsigned char) (fmd->getChecksum().getDataPtr()[i]));
                        }
                      }

                      if (printctime)
                      {
                        eos::FileMD::ctime_t ctime;
                        fmd->getCTime(ctime);
                        if (!printcounter)fprintf(fstdout, " ctime=%llu.%llu", (unsigned long long) ctime.tv_sec, (unsigned long long) ctime.tv_nsec);
                      }
                      if (printmtime)
                      {
                        eos::FileMD::ctime_t mtime;
                        fmd->getMTime(mtime);
                        if (!printcounter)fprintf(fstdout, " mtime=%llu.%llu", (unsigned long long) mtime.tv_sec, (unsigned long long) mtime.tv_nsec);
                      }

                      if (printrep)
                      {
                        if (!printcounter)fprintf(fstdout, " nrep=%d", (int) fmd->getNumLocation());
                      }

                      if (printunlink)
                      {
                        if (!printcounter)fprintf(fstdout, " nunlink=%d", (int) fmd->getNumUnlinkedLocation());
                      }

                      if (!printcounter)fprintf(fstdout, "\n");
                    }
                  }
                }
                if (selected)
                {
                  filecounter++;
                }
              }
              catch (eos::MDException &e)
              {
                eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
                gOFS->eosViewRWMutex.UnLockRead();
                //-------------------------------------------
              }
            }
            else
            {
              if (!printcounter)fprintf(fstdout, "%s\n", fspath.c_str());
              filecounter++;
            }
          }
          else
          {
            // get location
            //-------------------------------------------
            gOFS->eosViewRWMutex.LockRead();
            eos::FileMD* fmd = 0;
            try
            {
              fmd = gOFS->eosView->getFile(fspath.c_str());
            }
            catch (eos::MDException &e)
            {
              eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
            }

            if (fmd)
            {
              eos::FileMD fmdCopy(*fmd);
              fmd = &fmdCopy;
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------

              for (unsigned int i = 0; i < fmd->getNumLocation(); i++)
              {
                int loc = fmd->getLocation(i);
                size_t size = fmd->getSize();
                if (!loc)
                {
                  eos_err("fsid 0 found %s %llu", fmd->getName().c_str(), fmd->getId());
                  continue;
                }
                filesystembalance[loc] += size;

                if ((i == 0) && (size))
                {
                  int bin = (int) log10((double) size);
                  sizedistribution[ bin ] += size;
                  sizedistributionn[ bin ]++;
                }

                eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                eos::common::FileSystem* filesystem = 0;
                if (FsView::gFsView.mIdView.count(loc))
                {
                  filesystem = FsView::gFsView.mIdView[loc];
                }

                if (filesystem)
                {
                  eos::common::FileSystem::fs_snapshot_t fs;
                  if (filesystem->SnapShotFileSystem(fs, true))
                  {
                    spacebalance[fs.mSpace.c_str()] += size;
                    schedulinggroupbalance[fs.mGroup.c_str()] += size;
                  }
                }
              }
            }
            else
            {
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------
            }
          }
        }
      }
      gOFS->MgmStats.Add("FindEntries", pVid->uid, pVid->gid, cnt);
    }


    if ((option.find("d")) != STR_NPOS)
    {
      for (foundit = (*found).begin(); foundit != (*found).end(); foundit++)
      {
        // print directories
        XrdOucString attr = "";
        if (printkey.length())
        {
          gOFS->_attr_get(foundit->first.c_str(), *mError, vid, (const char*) 0, printkey.c_str(), attr);
          if (printkey.length())
          {
            if (!attr.length())
            {
              attr = "undef";
            }
            if (!printcounter)fprintf(fstdout, "%s=%-32s path=", printkey.c_str(), attr.c_str());
          }
        }
        if (!printcounter)
        {
          if (printchildcount)
          {
            //-------------------------------------------
            gOFS->eosViewRWMutex.LockRead();
            eos::ContainerMD* mCmd = 0;
            unsigned long long childfiles = 0;
            unsigned long long childdirs = 0;
            try
            {
              mCmd = gOFS->eosView->getContainer(foundit->first.c_str());
              childfiles = mCmd->getNumFiles();
              childdirs = mCmd->getNumContainers();
              gOFS->eosViewRWMutex.UnLockRead();
              fprintf(fstdout, "%s ndir=%llu nfiles=%llu\n", foundit->first.c_str(), childdirs, childfiles);
              //-------------------------------------------
            }
            catch (eos::MDException &e)
            {
              eos_debug("caught exception %d %s\n", e.getErrno(), e.getMessage().str().c_str());
              gOFS->eosViewRWMutex.UnLockRead();
              //-------------------------------------------
            }
          }
          else
          {
            fprintf(fstdout, "%s\n", foundit->first.c_str());
          }
        }
      }
      dircounter++;
    }
    if (deepquery)
    {
      globalfound->clear();
      deepQueryMutex.UnLock();
    }
    else
    {
      delete found;
    }
    if (printcounter)
    {
      fprintf(fstdout, "nfiles=%llu ndirectories=%llu\n", filecounter, dircounter);
    }
  }

  if (calcbalance)
  {
    XrdOucString sizestring = "";
    google::dense_hash_map<unsigned long, unsigned long long>::iterator it;
    for (it = filesystembalance.begin(); it != filesystembalance.end(); it++)
    {
      fprintf(fstdout, "fsid=%lu \tvolume=%-12s \tnbytes=%llu\n", it->first, eos::common::StringConversion::GetReadableSizeString(sizestring, it->second, "B"), it->second);
    }

    google::dense_hash_map<std::string, unsigned long long>::iterator its;
    for (its = spacebalance.begin(); its != spacebalance.end(); its++)
    {
      fprintf(fstdout, "space=%s \tvolume=%-12s \tnbytes=%llu\n", its->first.c_str(), eos::common::StringConversion::GetReadableSizeString(sizestring, its->second, "B"), its->second);
    }

    google::dense_hash_map<std::string, unsigned long long>::iterator itg;
    for (itg = schedulinggroupbalance.begin(); itg != schedulinggroupbalance.end(); itg++)
    {
      fprintf(fstdout, "sched=%s \tvolume=%-12s \tnbytes=%llu\n", itg->first.c_str(), eos::common::StringConversion::GetReadableSizeString(sizestring, itg->second, "B"), itg->second);
    }

    google::dense_hash_map<int, unsigned long long>::iterator itsd;
    for (itsd = sizedistribution.begin(); itsd != sizedistribution.end(); itsd++)
    {
      unsigned long long lowerlimit = 0;
      unsigned long long upperlimit = 0;
      if (((itsd->first) - 1) > 0)
        lowerlimit = pow10((itsd->first));
      if ((itsd->first) > 0)
        upperlimit = pow10((itsd->first) + 1);

      XrdOucString sizestring1;
      XrdOucString sizestring2;
      XrdOucString sizestring3;
      XrdOucString sizestring4;
      unsigned long long avgsize = (unsigned long long) (sizedistributionn[itsd->first] ? itsd->second / sizedistributionn[itsd->first] : 0);
      fprintf(fstdout, "sizeorder=%02d \trange=[ %-12s ... %-12s ] volume=%-12s \tavgsize=%-12s \tnbyptes=%llu \t avgnbytes=%llu \t nfiles=%llu\n", itsd->first
              , eos::common::StringConversion::GetReadableSizeString(sizestring1, lowerlimit, "B")
              , eos::common::StringConversion::GetReadableSizeString(sizestring2, upperlimit, "B")
              , eos::common::StringConversion::GetReadableSizeString(sizestring3, itsd->second, "B")
              , eos::common::StringConversion::GetReadableSizeString(sizestring4, avgsize, "B")
              , itsd->second
              , avgsize
              , sizedistributionn[itsd->first]
              );
    }
  }
  return SFS_OK;
}

EOSMGMNAMESPACE_END