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
#include "mgm/Macros.hh"
#include "mgm/Policy.hh"
#include "common/LayoutId.hh"
/*----------------------------------------------------------------------------*/
#include "XrdCl/XrdClCopyProcess.hh"
/*----------------------------------------------------------------------------*/
#include <math.h>
#include <memory>
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


    // -------------------------------------------------------------------------
    // change the number of stripes for files with replica layout
    // -------------------------------------------------------------------------
    if (mSubCmd == "layout")
    {
      XrdOucString stripes = pOpaque->Get("mgm.file.layout.stripes");
      int newstripenumber = 0;
      if (stripes.length()) newstripenumber = atoi(stripes.c_str());
      if (!stripes.length() ||
          ((newstripenumber < (eos::common::LayoutId::kOneStripe + 1)) ||
           (newstripenumber > (eos::common::LayoutId::kSixteenStripe + 1))))
      {
        stdErr = "error: you have to give a valid number of stripes"
                " as an argument to call 'file layout'";
        retc = EINVAL;
      }
      else
      {
        // only root can do that
        if (pVid->uid == 0)
        {
          eos::IFileMD* fmd = 0;
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
              eos_debug("caught exception %d %s\n",
                        e.getErrno(),
                        e.getMessage().str().c_str());
            }
          }
          else
          {
            // -----------------------------------------------------------------
            // reference by path
            // -----------------------------------------------------------------
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
              eos_debug("caught exception %d %s\n",
                        e.getErrno(),
                        e.getMessage().str().c_str());
            }
          }

          if (fmd)
          {
            if ((eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) ==
                 eos::common::LayoutId::kReplica) ||
                (eos::common::LayoutId::GetLayoutType(fmd->getLayoutId()) ==
                 eos::common::LayoutId::kPlain))
            {
              unsigned long newlayout =
                      eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica,
                                                   eos::common::LayoutId::GetChecksum(fmd->getLayoutId()),
                                                   newstripenumber,
                                                   eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId())
                                                   );

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
              stdErr = "error: you can only change the number of "
                      "stripes for files with replica layout";
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

    // -------------------------------------------------------------------------
    // verify checksum, size for files issuing an asynchronous verification req
    // -------------------------------------------------------------------------
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
        eos::IFileMD* fmd = 0;
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
          // -------------------------------------------------------------------
          // reference by fid+fsid
          // -------------------------------------------------------------------
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
            eos_debug("caught exception %d %s\n",
                      e.getErrno(),
                      e.getMessage().str().c_str());
          }
        }
        else
        {
          // -------------------------------------------------------------------
          // reference by path
          // -------------------------------------------------------------------
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
            eos_debug("caught exception %d %s\n",
                      e.getErrno(),
                      e.getMessage().str().c_str());
          }
        }

        if (fmd)
        {
          // -------------------------------------------------------------------
          // copy out the locations vector
          // -------------------------------------------------------------------
          eos::IFileMD::LocationVector::const_iterator it;
	  bool isRAIN=false;
          eos::IFileMD::LocationVector locations = fmd->getLocations();
	  eos::common::LayoutId::layoutid_t fmdlid = fmd->getLayoutId();
	  eos::common::FileId::fileid_t fileid = fmd->getId();

	  if ( (eos::common::LayoutId::GetLayoutType(fmdlid) ==
		eos::common::LayoutId::kRaidDP) ||
	       (eos::common::LayoutId::GetLayoutType(fmdlid) ==
		eos::common::LayoutId::kArchive) ||
	       (eos::common::LayoutId::GetLayoutType(fmdlid) ==
		eos::common::LayoutId::kRaid6)
	       ) 
	  {
	    isRAIN = true;
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

	    if (isRAIN) 
	    {
	      int lretc = gOFS->SendResync(fileid, (int) *it);
	      if (!lretc)
	      {
		stdOut += "success: sending resync for RAIN layout to fsid= ";
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
	    else
	    {
	      // rain layouts only resync meta data records
	      int lretc = gOFS->_verifystripe(spath.c_str(),
					      *mError,
					      vid,
					      (unsigned long) *it,
					      option);
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
	  }

          // -------------------------------------------------------------------
          // we want to be able to force the registration and verification of a
          // not registered replica
          // -------------------------------------------------------------------
          if (acceptfsid && (!acceptfound))
          {
            int lretc = gOFS->_verifystripe(spath.c_str(),
                                            *mError,
                                            vid,
                                            (unsigned long) acceptfsid,
                                            option
                                            );

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

    // -------------------------------------------------------------------------
    // move a replica/stripe from source fs to target fs
    // -------------------------------------------------------------------------
    if (mSubCmd == "move")
    {
      XrdOucString sfsidsource = pOpaque->Get("mgm.file.sourcefsid");
      unsigned long sourcefsid = (sfsidsource.length()) ?
              strtoul(sfsidsource.c_str(), 0, 10) : 0;
      XrdOucString sfsidtarget = pOpaque->Get("mgm.file.targetfsid");
      unsigned long targetfsid = (sfsidsource.length()) ?
              strtoul(sfsidtarget.c_str(), 0, 10) : 0;

      if (gOFS->_movestripe(spath.c_str(),
                            *mError,
                            *pVid,
                            sourcefsid,
                            targetfsid)
          )
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

    // -------------------------------------------------------------------------
    // replicate a replica/stripe from source fs to target fs
    // -------------------------------------------------------------------------
    if (mSubCmd == "replicate")
    {
      XrdOucString sfsidsource = pOpaque->Get("mgm.file.sourcefsid");
      unsigned long sourcefsid = (sfsidsource.length()) ?
              strtoul(sfsidsource.c_str(), 0, 10) : 0;
      XrdOucString sfsidtarget = pOpaque->Get("mgm.file.targetfsid");
      unsigned long targetfsid = (sfsidtarget.length()) ?
              strtoul(sfsidtarget.c_str(), 0, 10) : 0;

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

    // -------------------------------------------------------------------------
    // create URLs to share a file without authentication
    // -------------------------------------------------------------------------
    if (mSubCmd == "share")
    {
      XrdOucString sexpires = pOpaque->Get("mgm.file.expires");
      time_t expires = (sexpires.length()) ?
              (time_t) strtoul(sexpires.c_str(), 0, 10) : 0;

      if (!expires)
      {
        // default is 30 days
        expires = (time_t) (time(NULL) + (30 * 86400));
      }
     std::string sharepath;
      sharepath = gOFS->CreateSharePath(spath.c_str(), "", expires, *mError, *pVid);

      if (vid.uid != 0)
      {
        // non-root users cannot create shared URLs with validity > 90 days
        if ((expires - time(NULL)) > (90 * 86400))
        {
          stdErr += "error: you cannot request shared URLs with a validity longer than 90 days!\n";
          errno = EINVAL;
          retc = EINVAL;
          sharepath = "";
        }
      }

      if (!sharepath.length())
      {
        stdErr += "error: unable to create URLs for file sharing";
        retc = errno;
      }
      else
      {
        XrdOucString httppath = "http://";
        httppath += gOFS->HostName;
        httppath += ":8000/";

	size_t qpos = sharepath.find("?");
	std::string httpunenc = sharepath;
	httpunenc.erase(qpos);
	std::string httpenc = eos::common::StringConversion::curl_escaped(httpunenc);
	
	httppath += httpenc.c_str();

	XrdOucString cgi = sharepath.c_str();
	cgi.erase(0,qpos);
	while (cgi.replace("+", "%2B", qpos))
	{
	}

	httppath += cgi.c_str();

        XrdOucString rootUrl = "root://";
        rootUrl += gOFS->ManagerId;
        rootUrl += "/";
        rootUrl += sharepath.c_str();
        if (mHttpFormat)
        {
          stdOut += "<h4 id=\"sharevalidity\" >File Sharing Links: [ valid until  ";
          struct tm *newtime;
          newtime = localtime(&expires);
          stdOut += asctime(newtime);
          stdOut.erase(stdOut.length() - 1);
          stdOut += " ]</h4>\n";
          stdOut += path;
          stdOut += "<table border=\"0\"><tr><td>";
          stdOut += "<img alt=\"\" src=\"data:image/gif;base64,R0lGODlhEAANAJEAAAJ6xv///wAAAAAAACH5BAkAAAEALAAAAAAQAA0AAAg0AAMIHEiwoMGDCBMqFAigIYCFDBsadPgwAMWJBB1axBix4kGPEhN6HDgyI8eTJBFSvEgwIAA7\">";
          stdOut += "<a id=\"httpshare\" href=\"";
          stdOut += httppath.c_str();
          stdOut += "\">HTTP</a></td>";
          stdOut += "</tr><tr><td>";
          stdOut += "<img alt=\"\" src=\"data:image/gif;base64,R0lGODlhEAANAJEAAAJ6xv///wAAAAAAACH5BAkAAAEALAAAAAAQAA0AAAg0AAMIHEiwoMGDCBMqFAigIYCFDBsadPgwAMWJBB1axBix4kGPEhN6HDgyI8eTJBFSvEgwIAA7\">";
          stdOut += "<a id=\"rootshare\" href=\"";
          stdOut += rootUrl.c_str();
          stdOut += "\">ROOT</a></td>";
          stdOut += "</tr></table>\n";
        }
        else
        {
          stdOut += "[ root ]: ";
          stdOut += rootUrl;
          stdOut += "\n";
          stdOut += "[ http ]: ";
          stdOut += httppath;
          stdOut += "\n";
        }
      }
    }

    // -------------------------------------------------------------------------
    // rename a file or directory from source to target path
    // -------------------------------------------------------------------------
    if (mSubCmd == "rename")
    {
      XrdOucString source = pOpaque->Get("mgm.file.source");
      XrdOucString target = pOpaque->Get("mgm.file.target");

      if (gOFS->rename(source.c_str(), target.c_str(), *mError, *pVid, 0, 0, true))
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

    // -------------------------------------------------------------------------
    // link a file or directory from source to target path
    // -------------------------------------------------------------------------
    if (mSubCmd == "symlink")
    {
      XrdOucString source = pOpaque->Get("mgm.file.source");
      XrdOucString target = pOpaque->Get("mgm.file.target");

      if (gOFS->symlink(source.c_str(), target.c_str(), *mError, *pVid, 0, 0, true))
      {
        stdErr += "error: unable to link";
        retc = errno;
      }
      else
      {
        stdOut += "success: linked '";
        stdOut += source.c_str();
        stdOut += "' to '";
        stdOut += target.c_str();
        stdOut += "'";
      }
    }

    // -------------------------------------------------------------------------
    // tag/untag a file to be located on a certain file system
    // -------------------------------------------------------------------------
    if (mSubCmd == "tag")
    {
      if ( (! ((vid.prot == "sss") && (eos::common::Mapping::HasUid(DAEMONUID, vid.uid_list))) ) &&
	   (vid.uid) )
      {
	stdErr = "error: permission denied - you have to be root to run the 'tag' command";
	retc = EPERM;
	return SFS_OK;
      }

      XrdOucString sfsid = pOpaque->Get("mgm.file.tag.fsid");
      bool do_add=false;
      bool do_rm = false;
      bool do_unlink = false;

      if (sfsid.beginswith("+"))
      {
	do_add = true;
      }
      if (sfsid.beginswith("-"))
      {
	do_rm = true;
      }
      if (sfsid.beginswith("~"))
      {
	do_unlink = true;
      }

      sfsid.erase(0,1);
      errno = 0;
      int fsid = (sfsid.c_str())?(int)strtol(sfsid.c_str(),0,10):0;
      if (errno || fsid == 0 || (!do_add && !do_rm && !do_unlink))
      {
	stdErr = "error: you have to provide a valid filesystem id and a valid operation (+|-) e.g. 'file tag /myfile +1000'\n";
	retc = EINVAL;
	stdErr += sfsid;
      }
      else
      {
	eos::IFileMD* fmd = 0;
	{
	  eos::common::RWMutexWriteLock lock(gOFS->eosViewRWMutex);
	  try
	  {
	    fmd = gOFS->eosView->getFile(spath.c_str());
	    if (do_add && fmd->hasLocation(fsid))
	    {
	      stdErr += "error: file '";
	      stdErr += spath.c_str();
	      stdErr += "' is already located on fs=";
	      stdErr += (int) fsid;
	      retc = EINVAL;
	    }
	    else if ( ( do_rm || do_unlink) && (!fmd->hasLocation(fsid) && !fmd->hasUnlinkedLocation(fsid)) )
	    {
	      stdErr += "error: file '";
	      stdErr += spath.c_str();
	      stdErr += "' is not located on fs=";
	      stdErr += (int) fsid;
	      retc = EINVAL;
	    }
	    else
	    {
	      if (do_add)
		fmd->addLocation(fsid);

	      if (do_rm || do_unlink)
	      {
		fmd->unlinkLocation(fsid);
		if (do_rm)
		  fmd->removeLocation(fsid);
	      }

	      gOFS->eosView->updateFileStore(fmd);

	      if (do_add)
	      {
		stdOut += "success: added location to file '";
	      }
	      if (do_rm)
	      {
		stdOut += "success: removed location from file '";
	      }	
	      if (do_unlink)
	      {
		stdOut += "success: unlinked location from file '";
	      }	
	      stdOut += spath.c_str();
	      stdOut += "' on fs=";
	      stdOut += (int) fsid;
	    }
	  }
	  catch (eos::MDException &e)
	  {
	    errno = e.getErrno();
	    eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n", e.getErrno(), e.getMessage().str().c_str());
	  }
	  
	  if (!fmd)
	  {
	    stdErr += "error: unable to get file meta data of file '";
	    stdErr += spath.c_str();
	    stdErr += "'";
	    retc = errno;
	  }
	}
      }
    }

    // -------------------------------------------------------------------------
    // third-party copy files/directories
    // -------------------------------------------------------------------------
    if (mSubCmd == "copy")
    {
      XrdOucString src = spath;
      XrdOucString dst = pOpaque->Get("mgm.file.target");

      if (!dst.length())
      {
        stdErr += "error: missing destination argument";
        retc = EINVAL;
      }
      else
      {
        struct stat srcbuf;
        struct stat dstbuf;
        // check that we can access source and destination
        if (gOFS->_stat(src.c_str(), &srcbuf, *mError, *pVid, ""))
        {
          stdErr += "error: ";
          stdErr += mError->getErrText();
          retc = errno;
        }
        else
        {
          XrdOucString option = pOpaque->Get("mgm.file.option");
          bool silent = false;
          if ((option.find("s")) != STR_NPOS)
          {
            silent = true;
          }
          else
          {
            if ((option.find("c")) != STR_NPOS)
            {
              stdOut += "info: cloning '";
            }
            else
            {
              stdOut += "info: copying '";
            }
            stdOut += spath;
            stdOut += "' => '";
            stdOut += dst;
            stdOut += "' ...\n";
          }

          int dstat = gOFS->_stat(dst.c_str(), &dstbuf, *mError, *pVid, "");

          if ((option.find("f") == STR_NPOS) &&
              !dstat)
          {
            // there is no force flag and the target exists
            stdErr += "error: the target file exists - use '-f' to force the copy";
            retc = EEXIST;
          }
          else
          {
            // check source and destination access
            if (gOFS->_access(src.c_str(),
                              R_OK,
                              *mError,
                              *pVid,
                              "") ||
                gOFS->_access(dst.c_str(),
                              W_OK,
                              *mError,
                              *pVid,
                              ""
                              ))
            {
              stdErr += "error: ";
              stdErr += mError->getErrText();
              retc = errno;
            }
            else
            {
              std::vector<std::string> lCopySourceList;
              std::vector<std::string> lCopyTargetList;
              // ---------------------------------------------------------------
              // if this is a directory create a list of files to copy
              // ---------------------------------------------------------------
              std::map < std::string, std::set < std::string >> found;

              if (S_ISDIR(srcbuf.st_mode) && S_ISDIR(dstbuf.st_mode))
              {
                if (!gOFS->_find(src.c_str(), *mError, stdErr, *pVid, found))
                {
                  // -----------------------------------------------------------
                  // add all to the copy source,target list ...
                  // -----------------------------------------------------------

                  for (auto dirit = found.begin(); dirit != found.end(); dirit++)
                  {
                    // loop over dirs and add all the files
                    for (auto fileit = dirit->second.begin(); fileit != dirit->second.end(); fileit++)
                    {
                      std::string src_path = dirit->first;
                      std::string end_path = src_path;
                      end_path.erase(0, src.length());
                      src_path += *fileit;
                      std::string dst_path = dst.c_str();
                      dst_path += end_path;
                      dst_path += *fileit;
                      lCopySourceList.push_back(src_path.c_str());
                      lCopyTargetList.push_back(dst_path.c_str());
                      stdOut += "info: copying '";
                      stdOut += src_path.c_str();
                      stdOut += "' => '";
                      stdOut += dst_path.c_str();
                      stdOut += "' ... \n";
                    }
                  }
                }
                else
                {
                  stdErr += "error: find failed";
                }
              }
              else
              {
                // -------------------------------------------------------------
                // add a single file to the copy list
                // -------------------------------------------------------------
                lCopySourceList.push_back(src.c_str());
                lCopyTargetList.push_back(dst.c_str());
              }

              for (size_t i = 0; i < lCopySourceList.size(); i++)
              {
                // ---------------------------------------------------------------
                // setup a TPC job
                // ---------------------------------------------------------------
                XrdCl::PropertyList properties;
                XrdCl::PropertyList result;

                if (srcbuf.st_size)
                {
                  // TPC for non-empty files
                  properties.Set("thirdParty", "only");
                }

                properties.Set("force", true);
                properties.Set("posc", false);
                properties.Set("coerce", false);

                std::string source = lCopySourceList[i];
                std::string target = lCopyTargetList[i];
                std::string sizestring;
                std::string cgi = "eos.ruid=";
                cgi += eos::common::StringConversion::GetSizeString(sizestring,
                                                                    (unsigned long long) pVid->uid);
                cgi += "&eos.rgid=";
                cgi += eos::common::StringConversion::GetSizeString(sizestring,
                                                                    (unsigned long long) pVid->gid);
                cgi += "&eos.app=filecopy";

                if ((option.find("c")) != STR_NPOS)
                {
                  char clonetime[256];
                  snprintf(clonetime, sizeof (clonetime) - 1, "&eos.ctime=%lu&eos.mtime=%lu", srcbuf.st_ctime, srcbuf.st_mtime);
                  cgi += clonetime;
                }

                XrdCl::URL url_src;
                url_src.SetProtocol("root");
                url_src.SetHostName("localhost");
                url_src.SetUserName("root");
                url_src.SetParams(cgi);
                url_src.SetPath(source);

                XrdCl::URL url_trg;
                url_trg.SetProtocol("root");
                url_trg.SetHostName("localhost");
                url_trg.SetUserName("root");
                url_trg.SetParams(cgi);
                url_trg.SetPath(target);

                properties.Set("source", url_src);
                properties.Set("target", url_trg);
                properties.Set("sourceLimit", (uint16_t) 1);
                properties.Set("chunkSize", (uint32_t)(4 * 1024 * 1024));
                properties.Set("parallelChunks", (uint8_t) 1);

                XrdCl::CopyProcess lCopyProcess;
                lCopyProcess.AddJob(properties, &result);
                XrdCl::XRootDStatus lTpcPrepareStatus = lCopyProcess.Prepare();
                eos_static_info("[tpc]: %s=>%s %s",
                                url_src.GetURL().c_str(),
                                url_trg.GetURL().c_str(),
                                lTpcPrepareStatus.ToStr().c_str());
                
                if (lTpcPrepareStatus.IsOK())
                {
                  XrdCl::XRootDStatus lTpcStatus = lCopyProcess.Run(0);
                  eos_static_info("[tpc]: %s %d",
                                  lTpcStatus.ToStr().c_str(),
                                  lTpcStatus.IsOK());
                  if (lTpcStatus.IsOK())
                  {
                    if (!silent)
                    {
                      stdOut += "success: copy done '";
                      stdOut += source.c_str();
                      stdOut += "'\n";
                    }
                  }
                  else
                  {
                    stdErr += "error: copy failed ' ";
                    stdErr += source.c_str();
                    stdErr += "' - ";
                    stdErr += lTpcStatus.ToStr().c_str();

                    retc = EIO;
                  }
                }
                else
                {
                  stdErr += "error: copy failed - ";
                  stdErr += lTpcPrepareStatus.ToStr().c_str();
                  retc = EIO;
                }
              }
            }
          }
        }
      }
    }

    if (mSubCmd == "convert")
    {
      // -----------------------------------------------------------------------
      // check access permissions on source
      // -----------------------------------------------------------------------

      if ((gOFS->_access(spath.c_str(), W_OK, *mError, *pVid, "") != SFS_OK))
      {
        stdErr += "error: you have no write permission on '";
        stdErr += spath.c_str();
        stdErr += "'";
        retc = EPERM;
      }
      else
      {
        while (1)
        {
          XrdOucString layout = pOpaque->Get("mgm.convert.layout");
          XrdOucString space = pOpaque->Get("mgm.convert.space");
          XrdOucString plctplcy = pOpaque->Get("mgm.convert.placementpolicy");
          XrdOucString option = pOpaque->Get("mgm.option");

          //stdOut += ("Placement Policy is: " + plctplcy);
          if(plctplcy.length())
          {
            // -------------------------------------------------------------------
            // check that the placement policy is valid
            // "scattered" or "hybrid
            // i.e. scattered, hybrid:<geotag> or gathered:<geotag>
            // -------------------------------------------------------------------
            if(plctplcy=="scattered")
            {}
            else if(plctplcy.beginswith("hybrid:"))
            {}
            else if (plctplcy.beginswith("gathered:"))
            {}
            else
            {
              stdErr += "error: placement policy is invalid";
              retc = EINVAL;
              return SFS_OK;
            }
            plctplcy="~"+plctplcy;
          }
          else
            plctplcy="";
          if (!space.length())
          {
            // -------------------------------------------------------------------
            // retrieve the target space from the layout settings
            // -------------------------------------------------------------------
            eos::common::Path cPath(spath.c_str());
            eos::IContainerMD::XAttrMap map;
            int rc = gOFS->_attr_ls(cPath.GetParentPath(), *mError, *pVid, (const char *) 0, map);
            if (rc || (!map.count("sys.forced.space") && !map.count("user.forced.space")))
            {
              stdErr += "error: cannot get default space settings from parent directory attributes";
              retc = EINVAL;
            }
            else
            {
              if (map.count("sys.forced.space"))
                space = map["sys.forced.space"].c_str();
              else
                space = map["user.forced.space"].c_str();
            }
          }

          if (space.length())
          {

            if (!layout.length() && (option != "rewrite"))
            {
              stdErr += "error: conversion layout has to be defined";
              retc = EINVAL;
            }
            else
            {
              // get the file meta data
              eos::IFileMD* fmd = 0;
              int fsid = 0;

              eos::common::LayoutId::layoutid_t layoutid = 0;
              eos::common::FileId::fileid_t fileid = 0;
              {
                eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);
                try
                {
                  fmd = gOFS->eosView->getFile(spath.c_str());
                  layoutid = fmd->getLayoutId();
                  fileid = fmd->getId();
                  
                  if (fmd->getNumLocation())
                  {
                    eos::IFileMD::LocationVector loc_vect = fmd->getLocations();
                    fsid = *(loc_vect.begin());
                  }
                }
                catch (eos::MDException &e)
                {
                  errno = e.getErrno();
                  eos_debug("msg=\"exception\" ec=%d emsg=\"%s\"\n", e.getErrno(), e.getMessage().str().c_str());
                }
              }

              if (!fmd)
              {
                stdErr += "error: unable to get file meta data of file ";
                stdErr += spath.c_str();
                retc = errno;
              }
              else
              {
                char conversiontagfile[1024];

                if (option == "rewrite")
                {
                  stdOut += "info: rewriting file with identical layout id\n";
                  // we just rewrite the file as it was
                  char hexlayout[17];
                  snprintf(hexlayout, sizeof (hexlayout) - 1, "%08llx",
                           (long long) layoutid);
                  layout = hexlayout;
                  // get the space this file is currently hosted
                  if (!fsid)
                  {
                    // bummer, this file has not even a single replica
                    stdErr += "error: file has no replica attached\n";
                    retc = ENODEV;
                    break;
                  }

                  // figure out which space this fsid is in ...
                  {
                    eos::common::RWMutexReadLock lock(FsView::gFsView.ViewMutex);
                    FileSystem* filesystem = 0;
                    if (FsView::gFsView.mIdView.count((int) fsid))
                      filesystem = FsView::gFsView.mIdView[(int) fsid];
                    if (!filesystem)
                    {
                      stdErr += "error: couldn't find filesystem in view\n";
                      retc = EINVAL;
                      break;
                    }
                    // get the space of that filesystem
                    space = filesystem->GetString("schedgroup").c_str();
                    space.erase(space.find("."));
                    stdOut += "info:: rewriting into space '";
                    stdOut += space;
                    stdOut += "'\n";
                  }
                }

                if (eos::common::StringConversion::IsHexNumber(layout.c_str(), "%08x"))
                {
                  // we hand over as an conversion layout ID
                  snprintf(conversiontagfile,
                           sizeof (conversiontagfile) - 1,
                           "%s/%016llx:%s#%s",
                           gOFS->MgmProcConversionPath.c_str(),
                           fileid,
                           space.c_str(),
                           layout.c_str());
                  stdOut += "info: conversion based on hexadecimal layout id\n";
                }
                else
                {
                  unsigned long layout_type = 0;
                  unsigned long layout_stripes = 0;

                  // check if it was provided as <layout>:<stripes>
                  std::string lLayout = layout.c_str();
                  std::string lLayoutName;
                  std::string lLayoutStripes;
                  if (eos::common::StringConversion::SplitKeyValue(lLayout,
                                                                   lLayoutName,
                                                                   lLayoutStripes))
                  {
                    XrdOucString lLayoutString = "eos.layout.type=";
                    lLayoutString += lLayoutName.c_str();
                    lLayoutString += "&eos.layout.nstripes=";
                    lLayoutString += lLayoutStripes.c_str();
                    // ---------------------------------------------------------------
                    // add block checksumming and the default blocksize of 4 M
                    // ---------------------------------------------------------------

                    XrdOucEnv lLayoutEnv(lLayoutString.c_str());
                    layout_type =
                            eos::common::LayoutId::GetLayoutFromEnv(lLayoutEnv);
                    layout_stripes =
                            eos::common::LayoutId::GetStripeNumberFromEnv(lLayoutEnv);
                    // ---------------------------------------------------------------
                    // re-create layout id by merging in the layout stripes & type
                    // ---------------------------------------------------------------
                    layoutid =
                            eos::common::LayoutId::GetId(layout_type,
                                                         eos::common::LayoutId::kAdler,
                                                         layout_stripes,
                                                         eos::common::LayoutId::k4M,
                                                         eos::common::LayoutId::kCRC32C,
                                                         eos::common::LayoutId::GetRedundancyStripeNumber(layoutid));


                    snprintf(conversiontagfile,
                             sizeof (conversiontagfile) - 1,
                             "%s/%016llx:%s#%08lx%s",
                             gOFS->MgmProcConversionPath.c_str(),
                             fileid,
                             space.c_str(),
                             (unsigned long) layoutid,
                             plctplcy.c_str());
                    stdOut += "info: conversion based layout+stripe arguments\n";
                  }
                  else
                  {
                    // assume this is the name of an attribute
                    snprintf(conversiontagfile,
                             sizeof (conversiontagfile) - 1,
                             "%s/%016llx:%s#%s%s",
                             gOFS->MgmProcConversionPath.c_str(),
                             fileid,
                             space.c_str(),
                             layout.c_str(),
                             plctplcy.c_str());
                    stdOut += "info: conversion based conversion attribute name\n";
                  }
                }
                eos::common::Mapping::VirtualIdentity rootvid;
                eos::common::Mapping::Root(rootvid);
                if (gOFS->_touch(conversiontagfile, *mError, rootvid, 0))
                {
                  stdErr += "error: unable to create conversion job '";
                  stdErr += conversiontagfile;
                  stdErr += "'";
                  retc = errno;
                }
                else
                {
                  stdOut += "success: created conversion job '";
                  stdOut += conversiontagfile;
                  stdOut += "'";
                }
              }
            }
          }
          break; // while 1
        }
      }
    }

    // -------------------------------------------------------------------------
    // touch a file
    // -------------------------------------------------------------------------
    if (mSubCmd == "touch")
    {
      if (gOFS->_touch(spath.c_str(), *mError, *pVid, 0))
      {
        stdErr += "error: unable to touch";
        retc = errno;
      }
      else
      {
        stdOut += "success: touched '";
        stdOut += spath.c_str();
        stdOut += "'";
      }
    }

    // -------------------------------------------------------------------------
    // fix the current state of the file layout by removing/repairing or adding
    // replica/stripes
    // -------------------------------------------------------------------------
    if (mSubCmd == "adjustreplica")
    {
      // Only root can do this
      if (pVid->uid == 0)
      {
        eos::IFileMD* fmd = 0;
        bool nodrop = false;
	XrdOucString file_option = pOpaque->Get("mgm.file.option");

	if (file_option == "nodrop")
	  nodrop = true;

        // This flag indicates that the replicate command should queue
        // this transfers on the head of the FST transfer lists
        bool expressflag = false;
        XrdOucString file_express = pOpaque->Get("mgm.file.express");

        if (file_express == "1")
          expressflag = 1;

        int icreationsubgroup = -1;
        XrdOucString creationspace = pOpaque->Get("mgm.file.desiredspace");

        if (pOpaque->Get("mgm.file.desiredsubgroup"))
          icreationsubgroup = atoi(pOpaque->Get("mgm.file.desiredsubgroup"));

	gOFS->eosViewRWMutex.LockRead();

	// Reference by fid+fsid
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

          try
          {
            fmd = gOFS->eosFileService->getFileMD(fid);
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n",
                      e.getErrno(),
                      e.getMessage().str().c_str());
          }
        }
        else
        {
          // Reference by path
          try
          {
            fmd = gOFS->eosView->getFile(spath.c_str());
          }
          catch (eos::MDException &e)
          {
            errno = e.getErrno();
            stdErr = "error: cannot retrieve file meta data - ";
            stdErr += e.getMessage().str().c_str();
            eos_debug("caught exception %d %s\n",
                      e.getErrno(),
                      e.getMessage().str().c_str());
          }
        }

        XrdOucString space = "default";
        XrdOucString refspace = "";
        unsigned int forcedsubgroup = 0;

        if (fmd)
        {
          unsigned long long fid = fmd->getId();
          std::unique_ptr<eos::IFileMD> fmd_cpy{fmd->clone()};
          fmd = (eos::IFileMD*)(0);

          gOFS->eosViewRWMutex.UnLockRead();
          //-------------------------------------------

          // Check if that is a replica layout at all
          if (eos::common::LayoutId::GetLayoutType(fmd_cpy->getLayoutId()) ==
              eos::common::LayoutId::kReplica)
          {
            // Check the configured and available replicas
            XrdOucString sizestring;
            eos::IFileMD::LocationVector::const_iterator lociter;
            int nreplayout = eos::common::LayoutId::GetStripeNumber(fmd_cpy->getLayoutId()) + 1;
            int nrep = (int) fmd_cpy->getNumLocation();
            int nreponline = 0;
            int ngroupmix = 0;
            eos::IFileMD::LocationVector loc_vect = fmd_cpy->getLocations();
            
            for (lociter = loc_vect.begin(); lociter != loc_vect.end(); ++lociter)
            {
              // ignore filesystem id 0
              if (!(*lociter))
              {
                eos_err("fsid 0 found fid=%lld", fmd_cpy->getId());
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

                // Remember the spacename
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

                if ((snapshot.mConfigStatus > eos::common::FileSystem::kDrain) &&
                    (snapshot.mStatus == eos::common::FileSystem::kBooted))
                {
                  // This is an accessible replica
                  nreponline++;
                }
              }
            }

            eos_debug("path=%s nrep=%lu nrep-layout=%lu nrep-online=%lu",
                      spath.c_str(), nrep, nreplayout, nreponline);

            if (nreplayout > nreponline)
            {
              eos::common::RWMutexReadLock vlock(FsView::gFsView.ViewMutex);

              // Set the desired space & subgroup if provided
              if (creationspace.length())
                space = creationspace;

              if (icreationsubgroup != -1)
                forcedsubgroup = icreationsubgroup;

              // If the space is explicitly set, we don't force into a
	      // particular subgroup
              if (creationspace.length())
                forcedsubgroup = -1;

              // We don't have enough replica's online - trigger
	      // asynchronous replication
              int nnewreplicas = nreplayout - nreponline;

              eos_debug("forcedsubgroup=%d icreationsubgroup=%d",
                        forcedsubgroup, icreationsubgroup);

              // Get the location where we can read that file
              eos_debug("creating %d new replicas space=%s subgroup=%d",
                        nnewreplicas, space.c_str(), forcedsubgroup);

              // This defines the fs to use in the selectedfs vector
              unsigned long fsIndex;

              // Fill the existing locations
              std::vector<unsigned int> selectedfs;
              std::vector<unsigned int> unavailfs;
              std::vector<unsigned int> sourcefs = fmd_cpy->getLocations();
              std::string tried_cgi;

              // Now we just need to ask for <n> targets
              int layoutId = eos::common::LayoutId::GetId(eos::common::LayoutId::kReplica,
                                                          eos::common::LayoutId::kNone,
                                                          nnewreplicas);
              eos::common::Path cPath(spath.c_str());
              eos::IContainerMD::XAttrMap attrmap;
              gOFS->_attr_ls(cPath.GetParentPath(), *mError, *pVid, (const char *) 0,attrmap);
              eos::mgm::Scheduler::tPlctPolicy plctplcy;
              std::string targetgeotag;
              // Get placement policy
              Policy::GetPlctPolicy(spath.c_str(), attrmap, *pVid, *pOpaque,
                                    plctplcy, targetgeotag);

              // We don't know the container tag here, but we don't really
              // care since we are scheduled as root
              if (!(errno = Quota::FilePlacement(space.c_str(), spath.c_str(),
                                                 *pVid, 0, layoutId, sourcefs,
                                                 selectedfs, plctplcy, targetgeotag,
                                                 SFS_O_TRUNC, forcedsubgroup,
                                                 fmd_cpy->getSize())))
              {
                // We got a new replication vector
                for (unsigned int i = 0; i < selectedfs.size(); i++)
                {
                  if (!(errno = Quota::FileAccess(*pVid,
                                                  (unsigned long) 0,
                                                  space.c_str(),
                                                  tried_cgi,
                                                  (unsigned long) fmd_cpy->getLayoutId(),
                                                  sourcefs,
                                                  fsIndex,
                                                  false,
                                                  (long long unsigned) 0,
                                                  unavailfs)))
                  {
                    // This is now our source filesystem
                    unsigned int sourcefsid = sourcefs[fsIndex];

                    // stdOut += "info: replication := "; stdOut += (int) sourcefsid;
                    // stdOut += " => "; stdOut += (int)selectedfs[i]; stdOut += "\n";
                    // Add replication here
                    if (gOFS->_replicatestripe(fmd_cpy.get(), spath.c_str(),
                                               *mError, *pVid, sourcefsid,
                                               selectedfs[i], false, expressflag))
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
                  else
                  {
                    stdErr = "error: create new replicas => no source available: ";
                    stdErr += spath;
                    stdErr += "\n";
                    retc = ENOSPC;
                  }
                }
              }
              else
              {
                stdErr = "error: create new replicas => cannot place replicas: ";
                stdErr += spath;
                stdErr += "\n";
                retc = ENONET;
              }
            }
            else
            {
              // we do this only if we didn't create replicas in the section before,
              // otherwise we remove replicas which have used before for new replications

              // this is magic code to adjust the number of replicas to the desired policy ;-)
              if ((nreplayout < nrep) && (!nodrop))
              {
                std::vector<unsigned long> fsid2delete;
                unsigned int n2delete = nrep - nreplayout;

                // we build three views to sort the order of dropping

                std::multimap <int /*configstate*/, int /*fsid*/> statemap;
                std::multimap <std::string /*schedgroup*/, int /*fsid*/> groupmap;
                std::multimap <std::string /*space*/, int /*fsid*/> spacemap;

                // We have too many replica's online, we drop (nrepoonline-nreplayout)
                // replicas starting with the lowest configuration state
                eos_debug("trying to drop %d replicas space=%s subgroup=%d",
                          n2delete, creationspace.c_str(), icreationsubgroup);

                // Fill the views
                eos::IFileMD::LocationVector loc_vect = fmd_cpy->getLocations();
                for (lociter = loc_vect.begin(); lociter != loc_vect.end(); ++lociter)
                {
                  // ignore filesystem id 0
                  if (!(*lociter))
                  {
                    eos_err("fsid 0 found fid=%lld", fmd_cpy->getId());
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
                      for (stateit = statemap.begin();
                              stateit != statemap.end();
                              stateit++)
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

                    for (
                            lit = limitedstatemap.begin();
                            lit != limitedstatemap.end();
                            ++lit
                            )
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
                      for (stateit = statemap.begin();
                              stateit != statemap.end();
                              stateit++)
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
                  stdErr = "warning: cannot adjust replicas according to your "
                          "requirement: space=";
                  stdErr += creationspace;
                  stdErr += " subgroup=";
                  stdErr += icreationsubgroup;
                  stdErr += "\n";
                }

                for (unsigned int i = 0; i < fsid2delete.size(); i++)
                {
                  if (fmd_cpy->hasLocation(fsid2delete[i]))
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
                      stdErr = "error: drop excess replicas => cannot unlink "
                              "location - ";
                      stdErr += e.getMessage().str().c_str();
                      stdErr += "\n";
                      eos_debug("caught exception %d %s\n",
                                e.getErrno(),
                                e.getMessage().str().c_str());
                    }
                  }
                }
              }
            }
          }
	  else
	  {
	    // This is a rain layout, we try to rewrite the file using the converter
	    if ((eos::common::LayoutId::GetLayoutType(fmd_cpy->getLayoutId()) ==
		 eos::common::LayoutId::kRaidDP) ||
		(eos::common::LayoutId::GetLayoutType(fmd_cpy->getLayoutId()) ==
		 eos::common::LayoutId::kArchive) ||
		(eos::common::LayoutId::GetLayoutType(fmd_cpy->getLayoutId()) ==
		 eos::common::LayoutId::kRaid6))
	    {
	      ProcCommand Cmd;
	      // rewrite the file asynchronous using the converter
	      XrdOucString option = pOpaque->Get("mgm.option");
	      XrdOucString info;
	      info += "&mgm.cmd=file&mgm.subcmd=convert&mgm.option=rewrite&mgm.path=";
	      info += spath.c_str();
	      retc = Cmd.open("/proc/user", info.c_str(), *pVid, mError);
	      Cmd.AddOutput(stdOut, stdErr);
	      Cmd.close();
	      retc = Cmd.GetRetc();
	    }
	    else 
	    {
	      stdOut += "warning: no action for this layout type (neither replica nor rain)\n";
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

    // -------------------------------------------------------------------------
    // return meta data for a particular file
    // -------------------------------------------------------------------------
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
        eos::IFileMD* fmd = 0;
        //-------------------------------------------
        eos::common::RWMutexReadLock lock(gOFS->eosViewRWMutex);

	try
	{	
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
	    
	    fmd = gOFS->eosFileService->getFileMD(fid);
	  }
	  else 
	  {
	    fmd = gOFS->eosView->getFile(spath.c_str());
	  }
        }
        catch (eos::MDException &e)
        {
          errno = e.getErrno();
          stdErr = "error: cannot retrieve file meta data - ";
          stdErr += e.getMessage().str().c_str();
          eos_debug("caught exception %d %s\n",
                    e.getErrno(),
                    e.getMessage().str().c_str());
        }

        if (!fmd)
        {
          retc = errno;
        }
        else
        {
          XrdOucString sizestring;
          eos::IFileMD::LocationVector::const_iterator lociter;
          int i = 0;
          stdOut += "&";
          stdOut += "mgm.nrep=";
          stdOut += (int) fmd->getNumLocation();
          stdOut += "&";
          stdOut += "mgm.checksumtype=";
          stdOut += eos::common::LayoutId::GetChecksumString(fmd->getLayoutId());
          stdOut += "&";
          stdOut += "mgm.size=";
          stdOut +=
                  eos::common::StringConversion::GetSizeString(sizestring,
                                                               (unsigned long long) fmd->getSize());
          stdOut += "&";
          stdOut += "mgm.checksum=";
          size_t cxlen = eos::common::LayoutId::GetChecksumLen(fmd->getLayoutId());
          for (unsigned int i = 0; i < SHA_DIGEST_LENGTH; i++)
          {
            char hb[3];
            sprintf(hb, "%02x", (i < cxlen) ?
                    ((unsigned char) (fmd->getChecksum().getDataPadded(i))) : 0);
            stdOut += hb;
          }
          stdOut += "&";
          stdOut += "mgm.stripes=";
          stdOut += (int) (eos::common::LayoutId::GetStripeNumber(fmd->getLayoutId()) + 1);
          stdOut += "&";

          eos::IFileMD::LocationVector loc_vect = fmd->getLocations();
          for (lociter = loc_vect.begin(); lociter != loc_vect.end(); ++lociter)
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
              eos::common::FileId::FidPrefix2FullPath(hexstring.c_str(),
                                                      filesystem->GetPath().c_str(),
                                                      fullpath);
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

    // -------------------------------------------------------------------------
    // purge versions of a file
    // -------------------------------------------------------------------------
    if (mSubCmd == "purge")
    {
      XrdOucString max_count = pOpaque->Get("mgm.purge.version");
      ProcCommand Cmd;
      XrdOucString info;
      if (!max_count.length())
      {
        stdErr = "error: illegal version count specified";
        retc = EINVAL;
        return SFS_OK;
      }

      // stat this file
      struct stat buf;
      if (gOFS->_stat(spath.c_str(), &buf, *mError, *pVid, ""))
      {
        stdErr = "error; unable to stat path=";
        stdErr += spath.c_str();
        retc = errno;
        return SFS_OK;
      }


      info = "mgm.cmd=find&mgm.find.purge.versions=";
      info += max_count;
      info += "&mgm.path=";

      eos::common::Path cPath(spath.c_str());

      info += cPath.GetParentPath();
      info += "/.sys.v#.";
      info += cPath.GetName();
      info += "/";
      info += "&mgm.option=fMS";
      retc = Cmd.open("/proc/user", info.c_str(), *pVid, mError);
      Cmd.AddOutput(stdOut, stdErr);
      Cmd.close();
    }

    // -------------------------------------------------------------------------
    // create a new version of a file
    // -------------------------------------------------------------------------
    if (mSubCmd == "version")
    {
      XrdOucString max_count = pOpaque->Get("mgm.purge.version");
      int maxversion = 0;
      if (!max_count.length())
      {
        maxversion = -1;
      }
      else
      {
        maxversion = atoi(max_count.c_str());
        if (!maxversion)
        {
          stdErr = "error: illegal version count specified version-cnt=";
          stdErr += max_count.c_str();
          retc = EINVAL;
          return SFS_OK;
        }
      }


      // stat this file
      struct stat buf;
      if (gOFS->_stat(spath.c_str(), &buf, *mError, *pVid, ""))
      {
        stdErr = "error; unable to stat path=";
        stdErr += spath.c_str();
        retc = errno;
        return SFS_OK;
      }

      ProcCommand Cmd;
      // third party copy the file to a temporary name

      eos::common::Path atomicPath(spath.c_str());

      XrdOucString info;
      info += "&mgm.cmd=file&mgm.subcmd=copy&mgm.file.target=";
      info += atomicPath.GetAtomicPath(true);
      info += "&mgm.path=";
      info += spath.c_str();
      retc = Cmd.open("/proc/user", info.c_str(), *pVid, mError);
      Cmd.AddOutput(stdOut, stdErr);
      Cmd.close();

      if ((!Cmd.GetRetc()))
      {
	if (maxversion > 0) 
	{
	  XrdOucString versiondir;
	  eos::common::Path cPath(spath.c_str());
	  versiondir += cPath.GetParentPath();
	  versiondir += "/.sys.v#.";
	  versiondir += cPath.GetName();
	  versiondir += "/";

	  if (gOFS->PurgeVersion(versiondir.c_str(), *mError, maxversion))
	  {
	    stdErr += "error: unable to purge versions of path=";
	    stdErr += spath.c_str();
	    stdErr += "\n";
	    stdErr += "error: ";
	    stdErr += mError->getErrText();
	    retc = mError->getErrInfo();
	    return SFS_OK;
	  }
	}
	// everything worked well
	stdOut = "info: created new version of '";
	stdOut += spath.c_str();
	stdOut += "'";
	if (maxversion > 0)
	{
	  stdOut += " keeping ";
	  stdOut += (int)maxversion;
	  stdOut += " versions!";
	}
      }
    }

    // -------------------------------------------------------------------------
    // list or grab version(s) of a file
    // -------------------------------------------------------------------------
    if (mSubCmd == "versions")
    {
      XrdOucString grab = pOpaque->Get("mgm.grab.version");

      if (grab == "-1")
      {
        ProcCommand Cmd;
        // list versions
        eos::common::Path vpath(spath.c_str());
        XrdOucString info;
        info += "&mgm.cmd=ls&mgm.option=-l";
        info += "&mgm.path=";
        info += vpath.GetVersionDirectory();
        Cmd.open("/proc/user", info.c_str(), *pVid, mError);
        Cmd.AddOutput(stdOut, stdErr);
        Cmd.close();
        retc = Cmd.GetRetc();
        if (retc && (retc == ENOENT))
        {
          stdOut = "";
          stdErr = "error: no version exists for '";
          stdErr += spath.c_str();
          stdErr += "'";
          return SFS_OK;
        }
      }
      else
      {
        eos::common::Path vpath(spath.c_str());
        struct stat buf;
        struct stat vbuf;

        // stat this file

        if (gOFS->_stat(spath.c_str(), &buf, *mError, *pVid, ""))
        {
          stdErr = "error; unable to stat path=";
          stdErr += spath.c_str();
          retc = errno;
          return SFS_OK;
        }
        // grab version
        XrdOucString versionname = pOpaque->Get("mgm.grab.version");
        if (!versionname.length())
        {
          stdErr = "error: you have to provide the version you want to stage!";
          retc = EINVAL;
          return SFS_OK;
        }
        XrdOucString versionpath = vpath.GetVersionDirectory();
        versionpath += versionname;
        if (gOFS->_stat(versionpath.c_str(), &vbuf, *mError, *pVid, ""))
        {
          stdErr = "error: failed to stat your provided version path='";
          stdErr += versionpath;
          stdErr += "'";
          retc = errno;
          return SFS_OK;
        }

        // now stage a new version of the existing file
        XrdOucString versionedpath;
        if (gOFS->Version((eos::common::FileId::fileid_t)buf.st_ino >> 28, *mError, *pVid, -1, &versionedpath))
        {
          stdErr += "error: unable to create a version of path=";
          stdErr += spath.c_str();
          stdErr += "\n";
          stdErr += "error: ";
          stdErr += mError->getErrText();
          retc = mError->getErrInfo();
          return SFS_OK;
        }

        // and stage back the desired version
        if (gOFS->rename(versionpath.c_str(), spath.c_str(), *mError, *pVid))
        {
          stdErr += "error: unable to stage";
          stdErr += " '";
          stdErr += versionpath.c_str();
          stdErr += "' back to '";
          stdErr += spath.c_str();
          stdErr += "'";
          retc = errno;
          return SFS_OK;
        }
        else
        {
          stdOut += "success: staged '";
          stdOut += versionpath;
          stdOut += "' back to '";
          stdOut += spath.c_str();
          stdOut += "'";
          stdOut += " - the previous file is now '";
          stdOut += versionedpath;
          stdOut += ";";
        }
      }
    }
  }
  return SFS_OK;
}
EOSMGMNAMESPACE_END
