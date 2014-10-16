//------------------------------------------------------------------------------
// File: XrdFstOfsFile.cc
// Author: Elvin-Alin Sindrilaru / Andreas-Joachim Peters - CERN
//------------------------------------------------------------------------------

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
 * but WITHOUT ANY WARRANTY; without even the implied waDon'trranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

/*----------------------------------------------------------------------------*/
#include "common/Path.hh"
#include "common/http/OwnCloud.hh"
#include "fst/XrdFstOfsFile.hh"
#include "fst/XrdFstOfs.hh"
#include "fst/layout/LayoutPlugin.hh"
#include "fst/checksum/ChecksumPlugins.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOss/XrdOssApi.hh"
#include "XrdOuc/XrdOucIOVec.hh"
#include "XrdCl/XrdClXRootDResponses.hh"
/*----------------------------------------------------------------------------*/
#include <math.h>
/*----------------------------------------------------------------------------*/

extern XrdOssSys* XrdOfsOss;

EOSFSTNAMESPACE_BEGIN

  const uint16_t XrdFstOfsFile::msDefaultTimeout = 60; // default timeout value

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------

XrdFstOfsFile::XrdFstOfsFile (const char* user, int MonID) :
XrdOfsFile (user, MonID),
eos::common::LogId ()
{
  openOpaque = 0;
  capOpaque = 0;
  fstPath = "";
  hasBlockXs = false;
  closed = false;
  opened = false;
  haswrite = false;
  hasReadError = false;
  fMd = 0;
  checkSum = 0;
  layOut = 0;
  isRW = 0;
  isCreation = 0;
  commitReconstruction = 0;
  rBytes = wBytes = sFwdBytes = sBwdBytes = sXlFwdBytes = sXlBwdBytes = rOffset = wOffset = 0;
  rTime.tv_sec = lrTime.tv_sec = rvTime.tv_sec = lrvTime.tv_sec = 0;
  rTime.tv_usec = lrTime.tv_usec = rvTime.tv_usec = lrvTime.tv_usec = 0;
  wTime.tv_sec = lwTime.tv_sec = cTime.tv_sec = 0;
  wTime.tv_usec = lwTime.tv_usec = cTime.tv_usec = 0;
  fileid = 0;
  fsid = 0;
  lid = 0;
  cid = 0;
  rCalls = wCalls = nFwdSeeks = nBwdSeeks = nXlFwdSeeks = nXlBwdSeeks = 0;
  localPrefix = "";
  maxOffsetWritten = 0;
  openSize = 0;
  closeSize = 0;
  isReplication = false;
  isReconstruction = false;
  deleteOnClose = false;
  repairOnClose = false;
  closeTime.tv_sec = closeTime.tv_usec = 0;
  openTime.tv_sec = openTime.tv_usec = 0;
  tz.tz_dsttime = tz.tz_minuteswest = 0;
  viaDelete = remoteDelete = writeDelete = false;
  SecString = "";
  writeErrorFlag = 0;
  tpcFlag = kTpcNone;
  tpcState = kTpcIdle;
  ETag = "";
  mForcedMtime = 0;
  mForcedMtime_ms = 0;
  isOCchunk = 0;
}


//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------

XrdFstOfsFile::~XrdFstOfsFile ()
{
  viaDelete = true;

  if (!closed)
  {
    close();
  }

  if (openOpaque)
  {
    delete openOpaque;
    openOpaque = 0;
  }

  if (capOpaque)
  {
    delete capOpaque;
    capOpaque = 0;
  }

  //............................................................................
  // Unmap the MD record
  //............................................................................
  if (fMd)
  {
    delete fMd;
    fMd = 0;
  }

  if (checkSum)
  {
    delete checkSum;
    checkSum = 0;
  }

  if (layOut)
  {
    delete layOut;
    layOut = 0;
  }
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::openofs (const char* path,
                        XrdSfsFileOpenMode open_mode,
                        mode_t create_mode,
                        const XrdSecEntity* client,
                        const char* opaque)
{
  return XrdOfsFile::open(path, open_mode, create_mode, client, opaque);
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::open (const char* path,
                     XrdSfsFileOpenMode open_mode,
                     mode_t create_mode,
                     const XrdSecEntity* client,
                     const char* opaque)
{
  EPNAME("open");
  const char* tident = error.getErrUser();
  tIdent = error.getErrUser();
  char* val = 0;
  isRW = false;
  int retc = SFS_OK;
  Path = path;
  gettimeofday(&openTime, &tz);
  XrdOucString stringOpaque = opaque;
  XrdOucString opaqueCheckSum = "";
  std::string sec_protocol = client->prot;

  while (stringOpaque.replace("?", "&"))
  {
  }

  while (stringOpaque.replace("&&", "&"))
  {
  }

  int envlen;
  XrdOucString maskOpaque = opaque ? opaque : "";
  // mask some opaque parameters to shorten the logging
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.sym");
  eos::common::StringConversion::MaskTag(maskOpaque, "cap.msg");
  eos::common::StringConversion::MaskTag(maskOpaque, "authz");

  // For RAIN layouts if the opaque information contains the tag fst.store=1 the 
  // corrupted files are recovered back on disk. There is no other way to make
  // the distinction between an open for write an open for recovery since XrdCl
  // open in RDWR mode for both cases
  store_recovery = false;
  XrdOucEnv recvOpaque(stringOpaque.c_str());

  if ((val = recvOpaque.Get("fst.store")))
  {
    if (strncmp(val, "1", 1) == 0)
    {
      store_recovery = true;
      open_mode = SFS_O_RDWR;
      eos_info("msg=\"enabling RAIN store recovery\"");
    }
  }
  
  if ((open_mode & (SFS_O_WRONLY | SFS_O_RDWR | SFS_O_CREAT | SFS_O_TRUNC)) != 0)
  {
    isRW = true;
  }

  
  // ----------------------------------------------------------------------------
  // extract tpc keys
  // ----------------------------------------------------------------------------
  XrdOucEnv tmpOpaque(stringOpaque.c_str());

  SetLogId(0, client, tident);
  
  if ((val = tmpOpaque.Get("mgm.logid")))
  {
    SetLogId(val, client, tident);
  }

  if ((val = tmpOpaque.Get("mgm.etag")))
  {
    // extract our ETag from the redirection URL if available
    ETag = val;
  }

  if (eos::common::OwnCloud::isChunkUpload(tmpOpaque))
  {
    // tag as an OC chunk upload
    isOCchunk = true;
  }

  eos_info("path=%s info=%s isRW=%d open_mode=%x",
           Path.c_str(), maskOpaque.c_str(), isRW, open_mode);
  
  std::string tpc_stage = tmpOpaque.Get("tpc.stage") ?
    tmpOpaque.Get("tpc.stage") : "";
  std::string tpc_key = tmpOpaque.Get("tpc.key") ?
    tmpOpaque.Get("tpc.key") : "";
  std::string tpc_src = tmpOpaque.Get("tpc.src") ?
    tmpOpaque.Get("tpc.src") : "";
  std::string tpc_dst = tmpOpaque.Get("tpc.dst") ?
    tmpOpaque.Get("tpc.dst") : "";
  std::string tpc_org = tmpOpaque.Get("tpc.org") ?
    tmpOpaque.Get("tpc.org") : "";
  std::string tpc_lfn = tmpOpaque.Get("tpc.lfn") ?
    tmpOpaque.Get("tpc.lfn") : "";

  if (tpc_stage == "placement")
  {
    tpcFlag = kTpcSrcCanDo;
  }

  if (tpc_key.length())
  {
    time_t now = time(NULL);
    if ((tpc_stage == "placement") || (!gOFS.TpcMap[isRW].count(tpc_key.c_str())))
    {
      //.........................................................................
      // Create a TPC entry in the TpcMap 
      //.........................................................................
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      if (gOFS.TpcMap[isRW].count(tpc_key.c_str()))
      {
        //.......................................................................
        // TPC key replay go away
        //.......................................................................
        return gOFS.Emsg(epname, error, EPERM, "open - tpc key replayed", path);
      }
      if (tpc_key == "")
      {
        //.......................................................................
        // TPC key missing
        //.......................................................................
        return gOFS.Emsg(epname, error, EINVAL, "open - tpc key missing", path);
      }

      //.........................................................................
      // Compute the tpc origin e.g. <name>:<pid>@<host.domain>
      //.........................................................................

      // TODO: Xrootd 4.0      std::string origin_host = client->addrInfo->Name();
      std::string origin_host = client->host?client->host:"<sss-auth>";
      std::string origin_tident = client->tident;
      origin_tident.erase(origin_tident.find(":"));
      tpc_org = origin_tident;
      tpc_org += "@";
      tpc_org += origin_host;

      //.........................................................................
      // Store the TPC initialization
      //.........................................................................
      gOFS.TpcMap[isRW][tpc_key].key = tpc_key;
      gOFS.TpcMap[isRW][tpc_key].org = tpc_org;
      gOFS.TpcMap[isRW][tpc_key].src = tpc_src;
      gOFS.TpcMap[isRW][tpc_key].dst = tpc_dst;
      gOFS.TpcMap[isRW][tpc_key].path = path;
      gOFS.TpcMap[isRW][tpc_key].lfn = tpc_lfn;
      gOFS.TpcMap[isRW][tpc_key].opaque = stringOpaque.c_str();
      gOFS.TpcMap[isRW][tpc_key].expires = time(NULL) + 60; // one minute that's fine

      TpcKey = tpc_key.c_str();
      if (tpc_src.length())
      {
        // this is a destination session setup
        tpcFlag = kTpcDstSetup;
        if (!tpc_lfn.length())
        {
          return gOFS.Emsg(epname, error, EINVAL, "open - tpc lfn missing", path);
        }
      }
      else
      {
        // this is a source session setup
        tpcFlag = kTpcSrcSetup;
      }
      if (tpcFlag == kTpcDstSetup)
      {
        eos_info("msg=\"tpc dst session\" key=%s, org=%s, src=%s path=%s lfn=%s expires=%llu",
                 gOFS.TpcMap[isRW][tpc_key].key.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].org.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].src.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].path.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].lfn.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].expires);
      }
      else
      {
        eos_info("msg=\"tpc src session\" key=%s, org=%s, dst=%s path=%s expires=%llu",
                 gOFS.TpcMap[isRW][tpc_key].key.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].org.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].dst.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].path.c_str(),
                 gOFS.TpcMap[isRW][tpc_key].expires);
      }
    }
    else
    {
      //.........................................................................
      // Verify a TPC entry in the TpcMap 
      //.........................................................................

      // since the destination's open can now come before the transfer has been setup
      // we now have to give some time for the TPC client to deposit the key
      // the not so nice side effect is that this thread stays busy during that time

      bool exists = false;

      for (size_t i = 0; i < 150; i++)
      {
        XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
        if (gOFS.TpcMap[isRW].count(tpc_key))
          exists = true;
        if (!exists)
        {
          XrdSysTimer timer;
          timer.Wait(100);
        }
        else
        {
          break;
        }
      }

      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      if (!gOFS.TpcMap[isRW].count(tpc_key))
      {
        return gOFS.Emsg(epname, error, EPERM, "open - tpc key not valid", path);
      }
      if (gOFS.TpcMap[isRW][tpc_key].expires < now)
      {
        return gOFS.Emsg(epname, error, EPERM, "open - tpc key expired", path);
      }
      
      // we trust 'sss' anyway and we miss the host name in the 'sss' entity
      if ((sec_protocol != "sss") && (gOFS.TpcMap[isRW][tpc_key].org != tpc_org))
      {
        return gOFS.Emsg(epname, error, EPERM, "open - tpc origin mismatch", path);
      }
      //.........................................................................
      // Grab the open information
      //.........................................................................
      Path = gOFS.TpcMap[isRW][tpc_key].path.c_str();
      stringOpaque = gOFS.TpcMap[isRW][tpc_key].opaque.c_str();
      //.........................................................................
      // Expire TPC entry
      //.........................................................................
      gOFS.TpcMap[isRW][tpc_key].expires = (now - 10);

      // store the provided origin to compare with our local connection
      gOFS.TpcMap[isRW][tpc_key].org = tpc_org;
      // this must be a tpc read issued from a TPC target
      tpcFlag = kTpcSrcRead;
      TpcKey = tpc_key.c_str();
      eos_info("msg=\"tpc read\" key=%s, org=%s, path=%s expires=%llu",
               gOFS.TpcMap[isRW][tpc_key].key.c_str(),
               gOFS.TpcMap[isRW][tpc_key].org.c_str(),
               gOFS.TpcMap[isRW][tpc_key].src.c_str(),
               gOFS.TpcMap[isRW][tpc_key].path.c_str(),
               gOFS.TpcMap[isRW][tpc_key].expires);
    }
    //...........................................................................
    // Expire keys which are more than one 4 hours expired
    //...........................................................................
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    auto it = (gOFS.TpcMap[isRW]).begin();
    auto del = (gOFS.TpcMap[isRW]).begin();
    while (it != (gOFS.TpcMap[isRW]).end())
    {
      del = it;
      it++;
      if (now > (del->second.expires + (4 * 3600)))
      {
        eos_info("msg=\"expire tpc key\" key=%s", del->second.key.c_str());
        gOFS.TpcMap[isRW].erase(del);
      }
    }
  }

  stringOpaque += "&mgm.path=";
  stringOpaque += Path.c_str();
  openOpaque = new XrdOucEnv(stringOpaque.c_str());

  if ((val = openOpaque->Get("mgm.logid")))
  {
    SetLogId(val, tident);
  }

  if ((val = openOpaque->Get("mgm.checksum")))
  {
    opaqueCheckSum = val;
  }

  int caprc = 0;

  //.............................................................................
  // tpc src read can bypass capability checks
  //.............................................................................
  if ((tpcFlag != kTpcSrcRead) && (caprc = gCapabilityEngine.Extract(openOpaque, capOpaque)))
  {
    if (caprc == ENOKEY)
    {
      //.........................................................................
      // If we just miss the key, better stall the client
      //.........................................................................
      return gOFS.Stall(error, 10, "FST still misses the required capability key");
    }

    //...........................................................................
    // No capability - go away!
    //...........................................................................
    return gOFS.Emsg(epname, error, caprc, "open - capability illegal", Path.c_str());
  }
  else
  {
    if (tpcFlag == kTpcSrcRead)
    {
      //.........................................................................  
      // Grab the capability contents from the tpc key map
      //.........................................................................
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      if (gOFS.TpcMap[isRW][tpc_key].capability.length())
      {
        capOpaque = new XrdOucEnv(gOFS.TpcMap[isRW][tpc_key].capability.c_str());
      }
      else
      {
        return gOFS.Emsg(epname, error, EINVAL, "open - capability not found for tpc key %s", tpc_key.c_str());
      }
    }
    if (tpcFlag == kTpcSrcSetup)
    {
      //.........................................................................  
      // For a TPC setup we need to store the decoded capability contents
      //.........................................................................
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      gOFS.TpcMap[isRW][tpc_key].capability = capOpaque->Env(envlen);
    }
  }

  eos_info("capability=%s", capOpaque->Env(envlen));

  const char* hexfid = 0;
  const char* sfsid = 0;
  const char* slid = 0;
  const char* scid = 0;
  const char* smanager = 0;
  const char* sbookingsize = 0;
  const char* stargetsize = 0;
  bookingsize = 0;
  targetsize = 0;
  fileid = 0;
  fsid = 0;
  lid = 0;
  cid = 0;
  const char* secinfo = 0;

  if (!(hexfid = capOpaque->Get("mgm.fid")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no file id in capability", Path.c_str());
  }

  if (!(sfsid = capOpaque->Get("mgm.fsid")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no file system id in capability", Path.c_str());
  }

  if (!(secinfo = capOpaque->Get("mgm.sec")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no security information in capability", Path.c_str());
  }
  else
  {
    SecString = secinfo;
  }

  if ((val = capOpaque->Get("mgm.minsize")))
  {
    errno = 0;
    minsize = strtoull(val, 0, 10);

    if (errno)
    {
      eos_err("illegal minimum file size specified <%s>- restricting to 1 byte", val);
      minsize = 1;
    }
  }
  else
  {
    minsize = 0;
  }

  if ((val = capOpaque->Get("mgm.maxsize")))
  {
    errno = 0;
    maxsize = strtoull(val, 0, 10);

    if (errno)
    {
      eos_err("illegal maximum file size specified <%s>- restricting to 1 byte", val);
      maxsize = 1;
    }
  }
  else
  {
    maxsize = 0;
  }

  if ((val = openOpaque->Get("eos.pio.action")))
  {
    // figure out if this is a RAIN reconstruction
    XrdOucString action = val;
    if (action == "reconstruct")
    {
      haswrite = true;
      isReconstruction = true;
    }
  }

  //............................................................................
  // If we open a replica we have to take the right filesystem id and filesystem
  // prefix for that replica
  //............................................................................
  if (openOpaque->Get("mgm.replicaindex"))
  {
    XrdOucString replicafsidtag = "mgm.fsid";
    replicafsidtag += (int) atoi(openOpaque->Get("mgm.replicaindex"));

    if (capOpaque->Get(replicafsidtag.c_str()))
      sfsid = capOpaque->Get(replicafsidtag.c_str());
  }

  //............................................................................
  // Extract the local path prefix from the broadcasted configuration!
  //............................................................................
  eos::common::RWMutexReadLock lock(gOFS.Storage->fsMutex);
  fsid = atoi(sfsid ? sfsid : "0");

  if (fsid && gOFS.Storage->fileSystemsMap.count(fsid))
  {
    localPrefix = gOFS.Storage->fileSystemsMap[fsid]->GetPath().c_str();
  }

  //............................................................................
  // Attention: the localprefix implementation does not work for gateway machines
  // - this needs some modifications
  //............................................................................

  if (!localPrefix.length())
  {
    return gOFS.Emsg(epname, error, EINVAL,
                     "open - cannot determine the prefix path to use for the given filesystem id", Path.c_str());
  }

  if (!(slid = capOpaque->Get("mgm.lid")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no layout id in capability", Path.c_str());
  }

  if (!(scid = capOpaque->Get("mgm.cid")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no container id in capability", Path.c_str());
  }

  if (!(smanager = capOpaque->Get("mgm.manager")))
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - no manager name in capability", Path.c_str());
  }

  RedirectManager = smanager;
  int dpos = RedirectManager.find(":");

  if (dpos != STR_NPOS)
    RedirectManager.erase(dpos);

  eos::common::FileId::FidPrefix2FullPath(hexfid, localPrefix.c_str(), fstPath);
  fileid = eos::common::FileId::Hex2Fid(hexfid);
  fsid = atoi(sfsid);
  lid = (unsigned long)atoi(slid);
  cid = strtoull(scid, 0, 10);

  //............................................................................
  // Check if this is an open for replication
  //............................................................................
  eos_info("Path=%s beginswith=%d", Path.c_str(), Path.beginswith("/replicate:"));

  if (Path.beginswith("/replicate:"))
  {
    bool isopenforwrite = false;
    gOFS.OpenFidMutex.Lock();

    if (gOFS.WOpenFid[fsid].count(fileid))
    {
      if (gOFS.WOpenFid[fsid][fileid] > 0)
      {
        isopenforwrite = true;
      }
    }

    gOFS.OpenFidMutex.UnLock();

    if (isopenforwrite)
    {
      eos_err("forbid to open replica - file %s is opened in RW mode", Path.c_str());
      return gOFS.Emsg(epname, error, ENOENT,
                       "open - cannot replicate: file is opened in RW mode", Path.c_str());
    }

    isReplication = true;
  }

  if ((retc = XrdOfsOss->Stat(fstPath.c_str(), &updateStat)))
  {
    //..........................................................................
    // File does not exist, keep the create lfag
    //..........................................................................
    isCreation = true;
    openSize = 0;
    //..........................................................................
    // Used to indicate if a file was written in the meanwhile by someone else
    //..........................................................................
    updateStat.st_mtime = 0;
    // force the create flag
    open_mode |= SFS_O_CREAT;
    create_mode |= SFS_O_MKPTH;
  }
  else
  {
    eos_warning("removing creation flag because of %d %d", retc, errno);
    // remove the creat flag
    if (open_mode & SFS_O_CREAT)
      open_mode -= SFS_O_CREAT;
  }

  //............................................................................
  // Capability access distinction
  //............................................................................
  if (isRW)
  {
    if (isCreation)
    {
      if (!capOpaque->Get("mgm.access") 
	  || ( (strcmp(capOpaque->Get("mgm.access"), "create")) &&
               (strcmp(capOpaque->Get("mgm.access"), "write")) &&
	       (strcmp(capOpaque->Get("mgm.access"), "update")) ) )
      {
        return gOFS.Emsg(epname, 
                         error, 
                         EPERM, 
                         "open - capability does not allow to create/write/update this file", 
                         path);
      }
    }
    else
    {
      if (!capOpaque->Get("mgm.access") 
	  || ( (strcmp(capOpaque->Get("mgm.access"), "create")) &&
               (strcmp(capOpaque->Get("mgm.access"), "write")) &&
	       (strcmp(capOpaque->Get("mgm.access"), "update")) ) )
      {
        return gOFS.Emsg(epname, 
                         error, 
                         EPERM, 
                         "open - capability does not allow to update/write/create this file", 
                         path);
      }
    }
  }
  else
  {
    if (!capOpaque->Get("mgm.access") 
        || ((strcmp(capOpaque->Get("mgm.access"), "read")) &&
            (strcmp(capOpaque->Get("mgm.access"), "create")) &&
            (strcmp(capOpaque->Get("mgm.access"), "write")) &&
            (strcmp(capOpaque->Get("mgm.access"), "update"))))

    {
      return gOFS.Emsg(epname, 
                       error, 
                       EPERM, 
                       "open - capability does not allow to read this file", 
                       path);
    }
  }

  //............................................................................
  // Bookingsize is only needed for file creation
  //............................................................................
  if (isRW && isCreation)
  {
    if (!(sbookingsize = capOpaque->Get("mgm.bookingsize")))
    {
      return gOFS.Emsg(epname, error, EINVAL, "open - no booking size in capability", Path.c_str());
    }
    else
    {
      bookingsize = strtoull(capOpaque->Get("mgm.bookingsize"), 0, 10);

      if (errno == ERANGE)
      {
        eos_err("invalid bookingsize in capability bookingsize=%s", sbookingsize);
        return gOFS.Emsg(epname, error, EINVAL, "open - invalid bookingsize in capability", Path.c_str());
      }
    }

    if ((stargetsize = capOpaque->Get("mgm.targetsize")))
    {
      targetsize = strtoull(capOpaque->Get("mgm.targetsize"), 0, 10);

      if (errno == ERANGE)
      {
        eos_err("invalid targetsize in capability targetsize=%s", stargetsize);
        return gOFS.Emsg(epname, error, EINVAL, "open - invalid targetsize in capability", Path.c_str());
      }
    }
  }

  //............................................................................
  // Get the identity
  //............................................................................
  eos::common::Mapping::VirtualIdentity vid;
  eos::common::Mapping::Nobody(vid);

  if ((val = capOpaque->Get("mgm.ruid")))
  {
    vid.uid = atoi(val);
  }
  else
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec ruid missing", Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.rgid")))
  {
    vid.gid = atoi(val);
  }
  else
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec rgid missing", Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.uid")))
  {
    vid.uid_list.clear();
    vid.uid_list.push_back(atoi(val));
  }
  else
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec uid missing", Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.gid")))
  {
    vid.gid_list.clear();
    vid.gid_list.push_back(atoi(val));
  }
  else
  {
    return gOFS.Emsg(epname, error, EINVAL, "open - sec gid missing", Path.c_str());
  }

  if ((val = capOpaque->Get("mgm.logid")))
  {
    snprintf(logId, sizeof ( logId) - 1, "%s", val);
  }

  SetLogId(logId, vid, tident);
  eos_info("fstpath=%s", fstPath.c_str());

  //............................................................................
  // Get the layout object
  //............................................................................
  layOut = eos::fst::LayoutPlugin::GetLayoutObject(this, lid, client, &error,
                                                   eos::common::LayoutId::kLocal,
                                                   msDefaultTimeout, store_recovery);

  if (!layOut)
  {
    int envlen;
    eos_err("unable to handle layout for %s", capOpaque->Env(envlen));
    delete fMd;
    return gOFS.Emsg(epname, error, EINVAL, "open - illegal layout specified ",
                     capOpaque->Env(envlen));
  }

  layOut->SetLogId(logId, vid, tident);

  //............................................................................
  // Attach meta data
  //............................................................................
  fMd = gFmdDbMapHandler.GetFmd(fileid, fsid, vid.uid, vid.gid, lid, isRW);

  if (!fMd)
  {
    if (layOut->IsEntryServer() && (!isReplication))
    {
      eos_crit("no fmd for fileid %llu on filesystem %lu", fileid, fsid);
      int ecode = 1094;
      eos_warning("rebouncing client since we failed to get the FMD record back to MGM %s:%d",
                  RedirectManager.c_str(), ecode);
      return gOFS.Redirect(error, RedirectManager.c_str(), ecode);
    }
    else
    {
      eos_crit("no fmd for fileid %llu on filesystem %lu", fileid, (unsigned long long) fsid);
      return gOFS.Emsg(epname, error, ENOENT, "open - no FMD record found ");
    }
  }

  //............................................................................
  // Call the checksum factory function with the selected layout
  //............................................................................

  if (isRW || (opaqueCheckSum != "ignore"))
  {
    checkSum = eos::fst::ChecksumPlugins::GetChecksumObject(lid);
    eos_debug("checksum requested %d %u", checkSum, lid);
  }

  //............................................................................
  // Save block xs opaque information for the OSS layer
  //............................................................................
  if (eos::common::LayoutId::GetBlockChecksum(lid) != eos::common::LayoutId::kNone)
  {
    hasBlockXs = true;
  }

  XrdOucString oss_opaque = "";
  oss_opaque += "&mgm.lid=";
  oss_opaque += slid;
  oss_opaque += "&mgm.bookingsize=";
  oss_opaque += static_cast<int> (bookingsize);
  
  //............................................................................
  // Open layout implementation
  //............................................................................
  eos_info("fstpath=%s open-mode=%x create-mode=%x layout-name=%s", fstPath.c_str(), open_mode, create_mode, layOut->GetName());
  int rc = layOut->Open(fstPath.c_str(), open_mode, create_mode, oss_opaque.c_str());

  if ((!rc) && isCreation && bookingsize)
  {
    // ----------------------------------
    // check if the file system is full
    // ----------------------------------
    XrdSysMutexHelper(gOFS.Storage->fileSystemFullMapMutex);

    if (gOFS.Storage->fileSystemFullMap[fsid])
    {
      if (layOut->IsEntryServer() && (!isReplication))
      {
        writeErrorFlag = kOfsDiskFullError;
        layOut->Remove();
        int ecode = 1094;
        eos_warning("rebouncing client since we don't have enough space back to MGM %s:%d",
                    RedirectManager.c_str(), ecode);
        return gOFS.Redirect(error, RedirectManager.c_str(), ecode);
      }

      writeErrorFlag = kOfsDiskFullError;
      return gOFS.Emsg("writeofs", error, ENOSPC, "create file - disk space (headroom) exceeded fn=",
                       capOpaque ? (capOpaque->Get("mgm.path") ? capOpaque->Get("mgm.path") : FName()) : FName());
    }

    rc = layOut->Fallocate(bookingsize);

    if (rc)
    {
      eos_crit("file allocation gave return code %d errno=%d for allocation of size=%llu",
               rc, errno, bookingsize);

      if (layOut->IsEntryServer() && (!isReplication))
      {
        layOut->Remove();
        int ecode = 1094;
        eos_warning("rebouncing client since we don't have enough space back to MGM %s:%d",
                    RedirectManager.c_str(), ecode);
        return gOFS.Redirect(error, RedirectManager.c_str(), ecode);
      }
      else
      {
        layOut->Remove();
        return gOFS.Emsg(epname, error, ENOSPC, "open - cannot allocate required space", Path.c_str());
      }
    }
  }

  eos_info("checksum=%llx entryserver=%d", (unsigned long long) checkSum, layOut->IsEntryServer());

  if (!isCreation)
  {
    //..........................................................................
    // Get the real size of the file, not the local stripe size!
    //..........................................................................
    struct stat statinfo;

    if ((retc = layOut->Stat(&statinfo)))
    {
      return gOFS.Emsg(epname, error, EIO, "open - cannot stat layout to determine file size", Path.c_str());
    }

    //........................................................................
    // We feed the layout size, not the physical on disk!
    //........................................................................
    eos_info("msg=\"layout size\": disk_size=%zu db_size= %llu",
             statinfo.st_size, fMd->fMd.size());

    if ((off_t) statinfo.st_size != (off_t) fMd->fMd.size())
    {
      // in a RAID-like layout if the header is corrupted there is no way to know
      // the size of the initial file, therefore we take the value from the DB
      if (!isReconstruction)
      {
      openSize = fMd->fMd.size();
      }
      else
      {
        openSize = statinfo.st_size;
      }
    }
    else
    {
      openSize = statinfo.st_size;
    }

    if (checkSum && isRW)
    {
      //........................................................................
      // Preset with the last known checksum
      //........................................................................
      eos_info("msg=\"reset init\" file-xs=%s", fMd->fMd.checksum().c_str());
      checkSum->ResetInit(0, openSize, fMd->fMd.checksum().c_str());
    }
  }

  //.......................................................................................................
  // if we are not the entry server for ReedS & RaidDP layouts we disable the checksum object now for write
  // if we read we don't check checksums at all since we have block and parity checking
  //.......................................................................................................
  if (((eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kRaidDP) ||
       (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kRaid6) ||
       (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kArchive)) &&
      ((!isRW) || (!layOut->IsEntryServer())))
  {
    //........................................................................
    // This case we need to exclude!
    //........................................................................
    if (checkSum)
    {
      delete checkSum;
      checkSum = 0;
    }
  }

  std::string filecxerror = "0";

  if (!rc)
  {
    //........................................................................
    // Set the eos lfn as extended attribute
    //........................................................................
    eos::common::Attr* attr = eos::common::Attr::OpenAttr(layOut->GetLocalReplicaPath());

    if (attr && isRW)
    {
      if (Path.beginswith("/replicate:"))
      {
        if (capOpaque->Get("mgm.path"))
        {
          XrdOucString unsealedpath = capOpaque->Get("mgm.path");
          XrdOucString sealedpath = path;
          if (!attr->Set(std::string("user.eos.lfn"), std::string(unsealedpath.c_str())))
          {
            eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
          }
        }
        else
        {
          eos_err("no lfn in replication capability");
        }
      }
      else
      {
        if (!attr->Set(std::string("user.eos.lfn"), std::string(Path.c_str())))
        {
          eos_err("unable to set extended attribute <eos.lfn> errno=%d", errno);
        }
      }
    }

    //........................................................................
    // Try to get error if the file has a scan error
    //........................................................................
    if (attr)
    {
      filecxerror = attr->Get("user.filecxerror");
      delete attr;
    }
  }

  if ((!isRW) && (filecxerror == "1"))
  {
    //..........................................................................
    // If we have a replica layout
    //..........................................................................
    if (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kReplica)
    {
      //........................................................................
      // There was a checksum error during the last scan
      //........................................................................
      if (layOut->IsEntryServer() && (!isReplication))
      {
        int ecode = 1094;
        eos_warning("rebouncing client since our replica has a wrong checksum back to MGM %s:%d",
                    RedirectManager.c_str(), ecode);
        return gOFS.Redirect(error, RedirectManager.c_str(), ecode);
      }
      else
      {
        eos_err("open of %s failed - replica has a checksum mismatch", Path.c_str());
        return gOFS.Emsg(epname, error, EIO, "open - replica has a checksum mismatch", Path.c_str());
      }
    }
  }

  if (!rc)
  {
    opened = true;
    gOFS.OpenFidMutex.Lock();

    if (isRW)
    {
      gOFS.WOpenFid[fsid][fileid]++;
    }
    else
    {
      gOFS.ROpenFid[fsid][fileid]++;
    }

    gOFS.OpenFidMutex.UnLock();
  }
  else
  {
    //..........................................................................
    // If we have local errors in open we don't disable a filesystem -
    // this is done by the Scrub thread if necessary!
    //..........................................................................

    //..........................................................................
    // In any case we just redirect back to the manager if we are the 1st entry
    // point of the client
    //..........................................................................
    if (layOut->IsEntryServer() && (!isReplication))
    {
      int ecode = 1094;
      rc = SFS_REDIRECT;
      eos_warning("rebouncing client after open error back to MGM %s:%d", RedirectManager.c_str(), ecode);
      return gOFS.Redirect(error, RedirectManager.c_str(), ecode);
    }
    else
    {
      eos_warning("opening %s failed", Path.c_str());
      return gOFS.Emsg(epname, error, EIO, "open", Path.c_str());
    }
  }

  if (rc == SFS_OK)
  {
    //..........................................................................
    // Tag this transaction as open
    //..........................................................................
    if (isRW)
    {
      if (!gOFS.Storage->OpenTransaction(fsid, fileid))
      {
        eos_crit("cannot open transaction for fsid=%u fid=%llu", fsid, fileid);
      }
    }
  }

  eos_debug("open finished");
  return rc;
}


//------------------------------------------------------------------------------
// Compute total time to serve read requests
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadTime ()
{
  unsigned long mus = (lrTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrTime.tv_usec - cTime.tv_usec);
  rTime.tv_sec += (mus / 1000000);
  rTime.tv_usec += (mus % 1000000);
}


//------------------------------------------------------------------------------
// Compute total time to serve readV requests
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddReadVTime ()
{
  unsigned long mus = (lrvTime.tv_sec - cTime.tv_sec) * 1000000 +
                      (lrvTime.tv_usec - cTime.tv_usec);
  rvTime.tv_sec += (mus / 1000000);
  rvTime.tv_usec += (mus % 1000000);
}


//------------------------------------------------------------------------------
// Compute total time to serve write requests
//------------------------------------------------------------------------------
void
XrdFstOfsFile::AddWriteTime ()
{
  unsigned long mus = ((lwTime.tv_sec - cTime.tv_sec) * 1000000) +
    lwTime.tv_usec - cTime.tv_usec;
  wTime.tv_sec += (mus / 1000000);
  wTime.tv_usec += (mus % 1000000);
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

void
XrdFstOfsFile::MakeReportEnv (XrdOucString & reportString)
{
  // Compute avg, min, max, sigma for read and written bytes
  unsigned long long rmin, rmax, rsum;
  unsigned long long rvmin, rvmax, rvsum; // readv bytes
  unsigned long long rsmin, rsmax, rssum; // read single bytes
  unsigned long rcmin, rcmax, rcsum;      // readv count 
  unsigned long long wmin, wmax, wsum;
  double rsigma, rvsigma, rssigma, rcsigma, wsigma;
 
  {
    XrdSysMutexHelper vecLock(vecMutex);
    ComputeStatistics(rvec, rmin, rmax, rsum, rsigma);
    ComputeStatistics(wvec, wmin, wmax, wsum, wsigma);
    ComputeStatistics(monReadvBytes, rvmin, rvmax, rvsum, rvsigma);
    ComputeStatistics(monReadSingleBytes, rsmin, rsmax, rssum, rssigma);
    ComputeStatistics(monReadvCount, rcmin, rcmax, rcsum, rcsigma);

    char report[16384];
    snprintf(report, sizeof ( report) - 1,
             "log=%s&path=%s&ruid=%u&rgid=%u&td=%s&"
             "host=%s&lid=%lu&fid=%llu&fsid=%lu&"
             "ots=%lu&otms=%lu&"
             "cts=%lu&ctms=%lu&"
             "nrc=%lu&nwc=%lu&"
             "rb=%llu&rb_min=%llu&rb_max=%llu&rb_sigma=%.02f&"
             "rv_op=%llu&rvb_min=%llu&rvb_max=%llu&rvb_sum=%llu&rvb_sigma=%.02f&"
             "rs_op=%llu&rsb_min=%llu&rsb_max=%llu&rsb_sum=%llu&rsb_sigma=%.02f&"
             "rc_min=%lu&rc_max=%lu&rc_sum=%lu&rc_sigma=%.02f&"
             "wb=%llu&wb_min=%llu&wb_max=%llu&wb_sigma=%.02f&"
             "sfwdb=%llu&sbwdb=%llu&sxlfwdb=%llu&sxlbwdb=%llu"
             "nfwds=%lu&nbwds=%lu&nxlfwds=%lu&nxlbwds=%lu&"
             "rt=%.02f&rvt=%.02f&wt=%.02f&osize=%llu&csize=%llu&%s"
             , this->logId, Path.c_str(), this->vid.uid, this->vid.gid, tIdent.c_str()
             , gOFS.mHostName, lid, fileid, fsid
             , openTime.tv_sec, (unsigned long) openTime.tv_usec / 1000
             , closeTime.tv_sec, (unsigned long) closeTime.tv_usec / 1000
             , rCalls, wCalls
             , rsum, rmin, rmax, rsigma
             , (unsigned long long)monReadvBytes.size(), rvmin, rvmax, rvsum, rvsigma
             , (unsigned long long)monReadSingleBytes.size(), rsmin, rsmax, rssum, rssigma
             , rcmin, rcmax, rcsum, rcsigma
             , wsum
             , wmin
             , wmax
             , wsigma
             , sFwdBytes
             , sBwdBytes
             , sXlFwdBytes
             , sXlBwdBytes
             , nFwdSeeks
             , nBwdSeeks
             , nXlFwdSeeks
             , nXlBwdSeeks
             , ((rTime.tv_sec * 1000.0) + (rTime.tv_usec / 1000.0))
             , ((rvTime.tv_sec * 1000.0) + (rvTime.tv_usec / 1000.0))
             , ((wTime.tv_sec * 1000.0) + (wTime.tv_usec / 1000.0))
             , (unsigned long long) openSize
             , (unsigned long long) closeSize
             , eos::common::SecEntity::ToEnv(SecString.c_str(),
                                             ((tpcFlag == kTpcDstSetup) ||
                                              (tpcFlag == kTpcSrcRead)) ? "tpc" : 0).c_str());
    reportString = report;
  }
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::closeofs ()
{
  int rc = 0;

  bool fileExists = true;

  struct stat statinfo;
  if ((XrdOfsOss->Stat(fstPath.c_str(), &statinfo)))
  {
    fileExists = false;
  }

  // ---------------------------------------------------------------------------
  // check if the file could have been changed in the meanwhile ...
  // ---------------------------------------------------------------------------
  if (fileExists && isReplication && (!isRW))
  {
    // -------------------------------------------------------------------------
    gOFS.OpenFidMutex.Lock();
    if (gOFS.WOpenFid[fsid].count(fileid))
    {
      if (gOFS.WOpenFid[fsid][fileid] > 0)
      {
        eos_err("file is now open for writing -"
                " discarding replication [wopen=%d]",
                gOFS.WOpenFid[fsid][fileid]);

        gOFS.Emsg("closeofs",
                  error,
                  EIO,
                  "guarantee correctness - "
                  "file has been opened for writing during replication",
                  Path.c_str());

        rc = SFS_ERROR;
      }
    }
    gOFS.OpenFidMutex.UnLock();
    // -------------------------------------------------------------------------

    if ((statinfo.st_mtime != updateStat.st_mtime))
    {
      eos_err("file has been modified during replication");
      rc = SFS_ERROR;
      gOFS.Emsg("closeofs", error, EIO, "guarantee correctness -"
                "file has been modified during replication", Path.c_str());
    }
  }


  rc |= XrdOfsFile::close();
  return rc;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

bool
XrdFstOfsFile::verifychecksum ()
{
  bool checksumerror = false;
  int checksumlen = 0;

  //............................................................................
  // Deal with checksums
  //............................................................................
  if (checkSum)
  {
    checkSum->Finalize();

    if (checkSum->NeedsRecalculation())
    {
      if ((!isRW) && ((sFwdBytes + sBwdBytes)
                      || (checkSum->GetMaxOffset() != openSize)) && hasBlockXs)
      {
        //......................................................................
        // we don't rescan files if they are read non-sequential or only partially
        //......................................................................
        eos_debug("info=\"skipping checksum (re-scan) "
                  "for non-sequential reading ...\"");
        //......................................................................
        // remove the checksum object
        //......................................................................
        delete checkSum;
        checkSum = 0;
        return false;
      }
    }
    else
    {
      if (((!isRW) && (checkSum->GetMaxOffset() != openSize)))
      {
        eos_debug("info=\"skipping checksum (re-scan) for access without any IO or "
                  "partial sequential read IO from the beginning...\"");
        delete checkSum;
        checkSum = 0;
        return false;
      }
      if ((isRW) && checkSum->GetMaxOffset() && (checkSum->GetMaxOffset() < openSize))
      {
        // if there was a write which was not extending the file the checksum is dirty!
        checkSum->SetDirty();
      }
    }

    //............................................................................
    // if a checksum is not completely computed
    //............................................................................
    if (checkSum->NeedsRecalculation())
    {
      unsigned long long scansize = 0;
      float scantime = 0; // is ms

      if (!XrdOfsFile::fctl(SFS_FCTL_GETFD, 0, error))
      {
        int fd = error.getErrInfo();

        //......................................................................
        // rescan the file
        //......................................................................
        if (checkSum->ScanFile(fd, scansize, scantime))
        {
          XrdOucString sizestring;
          eos_info("info=\"rescanned checksum\" size=%s time=%.02f ms rate=%.02f MB/s %x",
                   eos::common::StringConversion::GetReadableSizeString(sizestring, scansize, "B"),
                   scantime,
                   1.0 * scansize / 1000 / (scantime ? scantime : 99999999999999LL),
                   checkSum->GetHexChecksum());
        }
        else
        {
          eos_err("Rescanning of checksum failed");
        }
      }
      else
      {
        eos_err("Couldn't get file descriptor");
      }
    }
    else
    {
      //........................................................................
      // This was prefect streaming I/O
      //........................................................................
      if ((!isRW) && (checkSum->GetMaxOffset() != openSize))
      {
        eos_info("info=\"skipping checksum (re-scan) since file was not read completely %llu %llu...\"",
                 checkSum->GetMaxOffset(), openSize);
        //......................................................................
        // Remove the checksum object
        //......................................................................
        delete checkSum;
        checkSum = 0;
        return false;
      }
    }

    if (isRW)
    {
      eos_info("(write) checksum type: %s checksum hex: %s requested-checksum hex: %s",
               checkSum->GetName(),
               checkSum->GetHexChecksum(),
               openOpaque->Get("mgm.checksum") ? openOpaque->Get("mgm.checksum") : "-none-");

      //........................................................................
      // Check if the check sum for the file was given at upload time
      //........................................................................
      if (openOpaque->Get("mgm.checksum"))
      {
        XrdOucString opaqueChecksum = openOpaque->Get("mgm.checksum");
        XrdOucString hexChecksum = checkSum->GetHexChecksum();

        if (opaqueChecksum != hexChecksum)
        {
          eos_err("requested checksum %s does not match checksum %s of uploaded file");
          delete checkSum;
          checkSum = 0;
          return true;
        }
      }

      checkSum->GetBinChecksum(checksumlen);
      //............................................................................
      // Copy checksum into meta data
      //............................................................................
      fMd->fMd.set_checksum(checkSum->GetHexChecksum());

      if (haswrite)
      {
        //............................................................................
        // If we have no write, we don't set this attributes (xrd3cp!)
        // set the eos checksum extended attributes
        //............................................................................
        eos::common::Attr* attr = eos::common::Attr::OpenAttr(fstPath.c_str());

        if (attr)
        {
          if (((eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kPlain) ||
               (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kReplica)))
          {
            //............................................................................
            // Don't put file checksum tags for complex layouts like raid6,readdp, archive
            //............................................................................

            if (!attr->Set(std::string("user.eos.checksumtype"), std::string(checkSum->GetName())))
            {
              eos_err("unable to set extended attribute <eos.checksumtype> errno=%d", errno);
            }

            if (!attr->Set("user.eos.checksum", checkSum->GetBinChecksum(checksumlen), checksumlen))
            {
              eos_err("unable to set extended attribute <eos.checksum> errno=%d", errno);
            }
          }
          //............................................................................
          // Reset any tagged error
          //............................................................................
          if (!attr->Set("user.eos.filecxerror", "0"))
          {
            eos_err("unable to set extended attribute <eos.filecxerror> errno=%d", errno);
          }

          if (!attr->Set("user.eos.blockcxerror", "0"))
          {
            eos_err("unable to set extended attribute <eos.blockcxerror> errno=%d", errno);
          }

          delete attr;
        }
      }
    }
    else
    {
      //............................................................................
      // This is a read with checksum check, compare with fMD
      //............................................................................
      bool isopenforwrite = false;

      // if the file is currently open to be written we don't check checksums!
      gOFS.OpenFidMutex.Lock();
      if (gOFS.WOpenFid[fsid].count(fileid))
      {
        if (gOFS.WOpenFid[fsid][fileid] > 0)
        {
          isopenforwrite = true;
        }
      }
      gOFS.OpenFidMutex.UnLock();

      if (isopenforwrite)
      {
        eos_info("(read)  disabling checksum check: file is currently written");
        return false;
      }

      eos_info("(read)  checksum type: %s checksum hex: %s fmd-checksum: %s",
               checkSum->GetName(),
               checkSum->GetHexChecksum(),
               fMd->fMd.checksum().c_str());
      std::string calculatedchecksum = checkSum->GetHexChecksum();

      if (calculatedchecksum != fMd->fMd.checksum().c_str())
      {
        checksumerror = true;
      }
    }
  }

  return checksumerror;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::close ()
{
  EPNAME("close");
  int rc = 0; // return code
  int brc = 0; // return code before 'close' has been called
  bool checksumerror = false;
  bool targetsizeerror = false;
  bool committed = false;
  bool minimumsizeerror = false;

  //.............................................................................
  // Any close on a file opened in TPC mode invalidates tpc keys
  if (TpcKey.length())
  {
    XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
    if (gOFS.TpcMap[isRW].count(TpcKey.c_str()))
    {
      eos_info("msg=\"remove tpc key\" key=%s", TpcKey.c_str());
      gOFS.TpcMap[isRW].erase(TpcKey.c_str());
      gOFS.TpcMap[isRW].resize(0);
    }
  }
  //............................................................................
  // We enter the close logic only once since there can be an explicit close or
  // a close via the destructor
  //............................................................................
  if (opened && (!closed) && fMd)
  {
    eos_info("");
    //..........................................................................
    // Check if the file close comes from a client disconnect e.g. the destructor
    //..........................................................................
    XrdOucString hexstring = "";
    eos::common::FileId::Fid2Hex(fMd->fMd.fid(), hexstring);
    XrdOucErrInfo error;
    XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
    XrdOucString OpaqueString = "";
    OpaqueString += "&mgm.fsid=";
    OpaqueString += (int) fMd->fMd.fsid();
    OpaqueString += "&mgm.fid=";
    OpaqueString += hexstring;
    XrdOucEnv Opaque(OpaqueString.c_str());
    capOpaqueString += OpaqueString;

    if ((viaDelete || writeDelete || remoteDelete) && isCreation)
    {
      //........................................................................
      // It is closed by the constructor e.g. no proper close
      // or the specified checksum does not match the computed one
      //........................................................................
      if (viaDelete)
      {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"client disconnect\"  fsid=%u fxid=%08x on fsid=%u ",
                 fMd->fMd.fsid(), fMd->fMd.fid());
      }

      if (writeDelete)
      {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"write/policy error\" fsid=%u fxid=%08x on fsid=%u ",
                 fMd->fMd.fsid(), fMd->fMd.fid());
      }

      if (remoteDelete)
      {
        eos_info("msg=\"(unpersist): deleting file\" reason=\"remote deletion\"    fsid=%u fxid=%08x on fsid=%u ",
                 fMd->fMd.fsid(), fMd->fMd.fid());
      }

      //........................................................................
      // Delete the file - set the file to be deleted
      //........................................................................
      deleteOnClose = true;
      layOut->Remove();
      //........................................................................
      // Delete the replica in the MGM
      //........................................................................
      int rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                capOpaque->Get("mgm.manager"), capOpaqueString);

      if (rc)
      {
        eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                    hexstring.c_str(), fMd->fMd.fid(), capOpaque->Get("mgm.manager"));
      }
    }
    else
    {
      //........................................................................
      // Check if this was a newly created file
      //........................................................................
      if (isCreation)
      {
        //......................................................................
        // If we had space allocation we have to truncate the allocated space to
        // the real size of the file
        //......................................................................
        if ((strcmp(layOut->GetName(), "raiddp") == 0) ||
            (strcmp(layOut->GetName(), "raid6") == 0) ||
            (strcmp(layOut->GetName(), "archive") == 0))
        {
          // the entry server has to truncate only if this is not a recovery action
          if (layOut->IsEntryServer() && !store_recovery)
          {
            eos_info("msg=\"truncate RAIN layout\" truncate-offset=%llu", (unsigned long long) maxOffsetWritten);
            layOut->Truncate(maxOffsetWritten);
          }
        }
        else
        {
          if ((long long) maxOffsetWritten > (long long) openSize)
          {
            //..................................................................
            // Check if we have to deallocate something for this file transaction
            //..................................................................
            if ((bookingsize) && (bookingsize > (long long) maxOffsetWritten))
            {
              eos_info("deallocationg %llu bytes", bookingsize - maxOffsetWritten);
              layOut->Truncate(maxOffsetWritten);
              //................................................................
              // We have evt. to deallocate blocks which have not been written
              //................................................................
              layOut->Fdeallocate(maxOffsetWritten, bookingsize);
            }
          }
        }
      }

      eos_info("calling verifychecksum");
      checksumerror = verifychecksum();
      targetsizeerror = (targetsize) ? (targetsize != (off_t) maxOffsetWritten) : false;

      if (isCreation)
      {
        // Check that the minimum file size policy is met!
        minimumsizeerror = (minsize) ? ((off_t) maxOffsetWritten < minsize) : false;

        if (minimumsizeerror)
        {
          eos_warning("written file %s is smaller than required minimum file size=%llu written=%llu",
                      Path.c_str(), minsize, maxOffsetWritten);
        }
      }

      if ((eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kRaidDP) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kRaid6) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kArchive))
      {
        //......................................................................
        // For RAID-like layouts don't do this check
        //......................................................................
        targetsizeerror = false;
        minimumsizeerror = false;
      }

      eos_debug("checksumerror = %i, targetsizerror= %i,"
                "maxOffsetWritten = %zu, targetsize = %lli",
                checksumerror, targetsizeerror, maxOffsetWritten, targetsize);

      //......................................................................
      // ---- add error simulation for checksum errors on read
      //......................................................................
      if ((!isRW) && gOFS.Simulate_XS_read_error)
      {
        checksumerror = true;
        eos_warning("simlating checksum errors on read");
      }

      //......................................................................
      // ---- add error simulation for checksum errors on write
      //......................................................................
      if (isRW && gOFS.Simulate_XS_write_error)
      {
        checksumerror = true;
        eos_warning("simlating checksum errors on write");
      }

      if (isCreation && (checksumerror || targetsizeerror || minimumsizeerror))
      {
        //......................................................................
        // We have a checksum error if the checksum was preset and does not match!
        // We have a target size error, if the target size was preset and does not match!
        //......................................................................
        // Set the file to be deleted
        //......................................................................
        deleteOnClose = true;
        layOut->Remove();
        //......................................................................
        // Delete the replica in the MGM
        //......................................................................
        int rc = gOFS.CallManager(&error,
                                  capOpaque->Get("mgm.path"),
                                  capOpaque->Get("mgm.manager"),
                                  capOpaqueString);

        if (rc)
        {
          eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                      hexstring.c_str(), fMd->fMd.fid(), capOpaque->Get("mgm.manager"));
        }
      }

      //........................................................................
      // Store the entry server information before closing the layout
      //........................................................................
      bool isEntryServer = false;

      if (layOut->IsEntryServer())
      {
        isEntryServer = true;
      }

      //........................................................................
      // First we assume that, if we have writes, we update it
      //........................................................................
      closeSize = openSize;

      if ((!checksumerror) && (haswrite || isCreation || commitReconstruction) && (!minimumsizeerror) && (!isReconstruction || !hasReadError))
      {
        //......................................................................
        // Commit meta data
        //......................................................................
        struct stat statinfo;

        if ((rc = layOut->Stat(&statinfo)))
        {
          rc = gOFS.Emsg(epname, error, EIO,
                         "close - cannot stat closed layout to determine file size",
                         Path.c_str());
        }

        if (!rc)
        {
          if ((statinfo.st_size == 0) || haswrite)
          {
            //..................................................................
            // Update size
            //..................................................................
            closeSize = statinfo.st_size;
            fMd->fMd.set_size(statinfo.st_size);
            fMd->fMd.set_disksize(statinfo.st_size);
            fMd->fMd.set_mgmsize(0xfffffffffff1ULL); // now again undefined
            fMd->fMd.set_mgmchecksum(""); // now again empty
            fMd->fMd.set_layouterror(0); // reset layout errors
            fMd->fMd.set_locations(""); // reset locations
            fMd->fMd.set_filecxerror(0);
            fMd->fMd.set_blockcxerror(0);
            fMd->fMd.set_locations(""); // reset locations
            fMd->fMd.set_filecxerror(0);
            fMd->fMd.set_blockcxerror(0);
            fMd->fMd.set_mtime(statinfo.st_mtime);
#ifdef __APPLE__
            fMd->fMd.set_mtime_ns(0);
#else
            fMd->fMd.set_mtime_ns(statinfo.st_mtim.tv_nsec);
#endif
            //..................................................................
            // Set the container id
            //..................................................................
            fMd->fMd.set_cid(cid);

            //..................................................................
            // For replicat's set the original uid/gid/lid values
            //..................................................................
            if (capOpaque->Get("mgm.source.lid"))
            {
              fMd->fMd.set_lid(strtoul(capOpaque->Get("mgm.source.lid"), 0, 10));
            }

            if (capOpaque->Get("mgm.source.ruid"))
            {
              fMd->fMd.set_uid(atoi(capOpaque->Get("mgm.source.ruid")));
            }

            if (capOpaque->Get("mgm.source.rgid"))
            {
              fMd->fMd.set_uid(atoi(capOpaque->Get("mgm.source.rgid")));
            }

            //..................................................................
            // Commit local
            //..................................................................
            if (!gFmdDbMapHandler.Commit(fMd))
              rc = gOFS.Emsg(epname, error, EIO,
                             "close - unable to commit meta data",
                             Path.c_str());

            //..................................................................
            // Commit to central mgm cache
            //..................................................................
            int envlen = 0;
            XrdOucString capOpaqueFile = "";
            XrdOucString mTimeString = "";
            capOpaqueFile += "/?";
            capOpaqueFile += capOpaque->Env(envlen);
            capOpaqueFile += "&mgm.pcmd=commit";
            capOpaqueFile += "&mgm.size=";
            char filesize[1024];
            sprintf(filesize, "%" PRIu64 "", fMd->fMd.size());
            capOpaqueFile += filesize;

            if (checkSum)
            {
              capOpaqueFile += "&mgm.checksum=";
              capOpaqueFile += checkSum->GetHexChecksum();
            }

            capOpaqueFile += "&mgm.mtime=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString, (unsigned long long) fMd->fMd.mtime());
            capOpaqueFile += "&mgm.mtime_ns=";
            capOpaqueFile += eos::common::StringConversion::GetSizeString(mTimeString, (unsigned long long) fMd->fMd.mtime_ns());
            capOpaqueFile += "&mgm.add.fsid=";
            capOpaqueFile += (int) fMd->fMd.fsid();

            //..................................................................
            // If <drainfsid> is set, we can issue a drop replica
            //..................................................................
            if (capOpaque->Get("mgm.drainfsid"))
            {
              capOpaqueFile += "&mgm.drop.fsid=";
              capOpaqueFile += capOpaque->Get("mgm.drainfsid");
            }

            if (isReconstruction)
            {
              //................................................................
              // indicate that this is a commit of a RAIN reconstruction
              //................................................................
              capOpaqueFile += "&mgm.reconstruction=1";
              if (!hasReadError && openOpaque->Get("eos.pio.recfs"))
              {
                capOpaqueFile += "&mgm.drop.fsid=";
                capOpaqueFile += openOpaque->Get("eos.pio.recfs");
                commitReconstruction = true;
              }
            }
            else
            {
              if (isEntryServer && !isReplication)
              {
                //..............................................................
                // The entry server commits size and checksum
                //..............................................................
                capOpaqueFile += "&mgm.commit.size=1&mgm.commit.checksum=1";
              }
              else
              {
                capOpaqueFile += "&mgm.replication=1";
              }
            }

            //..................................................................
            // The log ID to the commit
            //..................................................................
            capOpaqueFile += "&mgm.logid=";
            capOpaqueFile += logId;

            //..................................................................
            // Evt. tag as an OC-Chunk commit
            //..................................................................
            if (isOCchunk)
            {
              // add the chunk information
              int envlen;
              capOpaqueFile += eos::common::OwnCloud::FilterOcQuery(openOpaque->Env(envlen));
            }

            rc = gOFS.CallManager(&error, capOpaque->Get("mgm.path"),
                                  capOpaque->Get("mgm.manager"), capOpaqueFile);

            if (rc)
            {
              if ((rc == -EIDRM) || (rc == -EBADE) || (rc == -EBADR))
              {
                if (!gOFS.Storage->CloseTransaction(fsid, fileid))
                {
                  eos_crit("cannot close transaction for fsid=%u fid=%llu", fsid, fileid);
                }

                if (rc == -EIDRM)
                {
                  //..............................................................
                  // This file has been deleted in the meanwhile ... we can
                  // unlink that immedeatly
                  //..............................................................
                  eos_info("info=\"unlinking fid=%08x path=%s - "
                           "file has been already unlinked from the namespace\"",
                           fMd->fMd.fid(), Path.c_str());
                }

                if (rc == -EBADE)
                {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "file size of replica does not match reference\"",
                          fMd->fMd.fid(), Path.c_str());
                }

                if (rc == -EBADR)
                {
                  eos_err("info=\"unlinking fid=%08x path=%s - "
                          "checksum of replica does not match reference\"",
                          fMd->fMd.fid(), Path.c_str());
                }

                deleteOnClose = true;
              }
              else
              {
                eos_crit("commit returned an uncatched error msg=%s [probably timeout] - closing transaction to keep the file save", error.getErrText());
                if (isRW)
                  gOFS.Storage->CloseTransaction(fsid, fileid);
              }
            }
            else
            {
              committed = true;
            }
          }
        }
      }
    }

    if (isRW)
    {
      if (rc == SFS_OK)
      {
        gOFS.Storage->CloseTransaction(fsid, fileid);
      }
    }


    // ----------------------------------------------------------------------------------------------------------------------
    // recompute our ETag
    // ----------------------------------------------------------------------------------------------------------------------
    {
      // if there is a checksum we use the checksum, otherwise we return inode+mtime
      if (checkSum)
      {

        if (strcmp(checkSum->GetName(), "md5"))
        {
          // use inode + checksum
          char setag[256];
          snprintf(setag, sizeof (setag) - 1, "\"%llu:%s\"", eos::common::FileId::FidToInode((unsigned long long) fMd->fMd.fid()), fMd->fMd.checksum().c_str());
          ETag = setag;
        }
        else
        {
          // use checksum, S3 wants the pure MD5
          char setag[256];
          snprintf(setag, sizeof (setag) - 1, "\"%s\"", fMd->fMd.checksum().c_str());
          ETag = setag;
        }
      }
      else
      {
        // use inode + mtime
        char setag[256];
        snprintf(setag, sizeof (setag) - 1, "\"%llu:%llu\"", eos::common::FileId::FidToInode((unsigned long long) fMd->fMd.fid()), (unsigned long long) fMd->fMd.mtime());
        ETag = setag;
      }
    }

    int closerc = 0; // return of the close
    brc = rc; // return before the close

    if (layOut)
    {
      closerc = layOut->Close();
      rc |= closerc;
    }
    else
    {
      rc |= closeofs();
    }

    closed = true;

    if (closerc || (isReconstruction && hasReadError))
    {
      // For RAIN layouts if there is an error on close when writing then we 
      // delete the whole file
      // If we do RAIN reconstruction we cleanup this local replica which was not commited
      if ((eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kRaidDP) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kRaid6) ||
          (eos::common::LayoutId::GetLayoutType(layOut->GetLayoutId()) == eos::common::LayoutId::kArchive))
      {
        deleteOnClose = true;
      }
      else
      {
        // Some (remote) replica didn't make it through ... trigger an auto-repair
        if (!deleteOnClose)
        {
          repairOnClose = true;
        }
      }
    }

    gOFS.OpenFidMutex.Lock();

    if (isRW)
      gOFS.WOpenFid[fMd->fMd.fsid()][fMd->fMd.fid()]--;
    else
      gOFS.ROpenFid[fMd->fMd.fsid()][fMd->fMd.fid()]--;

    if (gOFS.WOpenFid[fMd->fMd.fsid()][fMd->fMd.fid()] <= 0)
    {
      //........................................................................
      // If this was a write of the last writer we had the lock and we release it
      //........................................................................
      gOFS.WOpenFid[fMd->fMd.fsid()].erase(fMd->fMd.fid());
      gOFS.WOpenFid[fMd->fMd.fsid()].resize(0);
    }

    if (gOFS.ROpenFid[fMd->fMd.fsid()][fMd->fMd.fid()] <= 0)
    {
      gOFS.ROpenFid[fMd->fMd.fsid()].erase(fMd->fMd.fid());
      gOFS.ROpenFid[fMd->fMd.fsid()].resize(0);
    }

    gOFS.OpenFidMutex.UnLock();
    gettimeofday(&closeTime, &tz);

    if (!deleteOnClose)
    {
      //.........................................................................
      // Prepare a report and add to the report queue
      //.........................................................................
      if ((tpcFlag != kTpcSrcSetup) && (tpcFlag != kTpcSrcCanDo))
      {
        //.......................................................................
        // We don't want a report for the source tpc setup or can do open
        //.......................................................................
        XrdOucString reportString = "";
        MakeReportEnv(reportString);
        gOFS.ReportQueueMutex.Lock();
        gOFS.ReportQueue.push(reportString);
        gOFS.ReportQueueMutex.UnLock();
      }
      if (isRW)
      {
        //.......................................................................
        // Store in the WrittenFilesQueue
        //.......................................................................
        gOFS.WrittenFilesQueueMutex.Lock();
        gOFS.WrittenFilesQueue.push(fMd->fMd);
        gOFS.WrittenFilesQueueMutex.UnLock();
      }
    }

    // ---------------------------------------------------------------------------
    // check if the target filesystem has been put into some non-operational mode 
    // in the meanwhile, it makes no sense to try to commit in this case
    // ---------------------------------------------------------------------------
    {
      eos::common::RWMutexReadLock lock(gOFS.Storage->fsMutex);
      if (gOFS.Storage->fileSystemsMap.count(fsid) && gOFS.Storage->fileSystemsMap[fsid]->GetConfigStatus() <
          eos::common::FileSystem::kDrain)
      {

        eos_notice("msg=\"failing transfer because filesystem has non-operational state\" path=%s state=%s", Path.c_str(), eos::common::FileSystem::GetConfigStatusAsString(gOFS.Storage->fileSystemsMap[fsid]->GetConfigStatus()));
        deleteOnClose = true;
      }
    }

    if (deleteOnClose && isCreation)
    {
      eos_info("info=\"deleting on close\" fn=%s fstpath=%s",
               capOpaque->Get("mgm.path"), fstPath.c_str());
      int retc = gOFS._rem(Path.c_str(), error, 0, capOpaque, fstPath.c_str(),
                           fileid, fsid, true);

      if (retc)
      {
        eos_debug("<rem> returned retc=%d", retc);
      }

      if (committed)
      {
        //..................................................................................
        // if we committed the replica and an error happened remote, we have to unlink it again
        //..................................................................................
        XrdOucString hexstring = "";
        eos::common::FileId::Fid2Hex(fileid, hexstring);
        XrdOucErrInfo error;
        XrdOucString capOpaqueString = "/?mgm.pcmd=drop";
        XrdOucString OpaqueString = "";
        OpaqueString += "&mgm.fsid=";
        OpaqueString += (int) fsid;
        OpaqueString += "&mgm.fid=";
        OpaqueString += hexstring;

        // If deleteOnClose at the gateway then we drop all replicas
        if (layOut->IsEntryServer())
        {
          OpaqueString += "&mgm.dropall=1";
        }

        XrdOucEnv Opaque(OpaqueString.c_str());
        capOpaqueString += OpaqueString;
        //..................................................................................
        // delete the replica in the MGM
        //..................................................................................
        int rcode = gOFS.CallManager(&error, capOpaque->Get("mgm.path"), capOpaque->Get("mgm.manager"), capOpaqueString);

        if (rcode)
        {
          if (rcode != -EIDRM)
          {
            eos_warning("(unpersist): unable to drop file id %s fsid %u at manager %s",
                        hexstring.c_str(), fileid, capOpaque->Get("mgm.manager"));
          }
        }

        eos_info("info=\"removing on manager\" manager=%s fid=%llu fsid=%d fn=%s fstpath=%s rc=%d",
                 capOpaque->Get("mgm.manager"),
                 (unsigned long long) fileid,
                 (int) fsid,
                 capOpaque->Get("mgm.path"),
                 fstPath.c_str(),
                 rcode);
      }

      rc = SFS_ERROR;

      if (minimumsizeerror)
      {
        //..................................................................................
        // minimum size criteria not fullfilled
        //..................................................................................
        gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because it is smaller than the required minimum file size in that directory", Path.c_str());
        eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason=\"minimum file size criteria\"", capOpaque->Get("mgm.path"), fstPath.c_str());
      }
      else
      {
        if (checksumerror)
        {
          //..................................................................................
          // checksum error
          //..................................................................................
          gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because of a checksum error ", Path.c_str());
          eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason=\"checksum error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
        }
        else
        {
          if (writeErrorFlag == kOfsSimulatedIoError)
          {
            //.................................................................................
            // simulted write error
            //..................................................................................
            gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because of a simulated IO error ", Path.c_str());
            eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason=\"simulated IO error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
          }
          else
          {
            if (writeErrorFlag == kOfsMaxSizeError)
            {
              //..................................................................................
              // maximum size criteria not fullfilled
              //..................................................................................
              gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because you exceeded the maximum file size settings for this namespace branch", Path.c_str());
              eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason=\"maximum file size criteria\"", capOpaque->Get("mgm.path"), fstPath.c_str());
            }
            else
            {
              if (writeErrorFlag == kOfsDiskFullError)
              {
                //..................................................................................
                // disk full detected during write
                //..................................................................................
                gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because the target disk filesystem got full and you didn't use reservation", Path.c_str());
                eos_warning("info=\"deleting on close\" fn=%s fstpath=%s reason=\"filesystem full\"", capOpaque->Get("mgm.path"), fstPath.c_str());
              }
              else
              {
                if (writeErrorFlag == kOfsIoError)
                {
                  //..................................................................................
                  // generic IO error on the underlying device
                  //..................................................................................
                  gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because of an IO error during a write operation", Path.c_str());
                  eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason=\"write IO error\"", capOpaque->Get("mgm.path"), fstPath.c_str());
                }
                else
                {
                  //..................................................................................
                  // target size is different from the uploaded file size
                  //..................................................................................
                  if (targetsizeerror)
                  {
                    gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because the stored file does not match the provided targetsize", Path.c_str());
                    eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason=\"target size mismatch\"", capOpaque->Get("mgm.path"), fstPath.c_str());
                  }
                  else
                  {
                    //..................................................................................
                    // client has disconnected and file is cleaned-up
                    //..................................................................................
                    gOFS.Emsg(epname, error, EIO, "store file - file has been cleaned because of a client disconnect", Path.c_str());
                    eos_crit("info=\"deleting on close\" fn=%s fstpath=%s reason=\"client disconnect\"", capOpaque->Get("mgm.path"), fstPath.c_str());
                  }
                }
              }
            }
          }
        }
      }
    }
    else
    {
      if (checksumerror)
      {
        //..................................................................................
        // checksum error detected
        //..................................................................................
        rc = SFS_ERROR;
        gOFS.Emsg(epname, error, EIO, "verify checksum - checksum error for file fn=", capOpaque->Get("mgm.path"));
        int envlen = 0;
        eos_crit("file-xs error file=%s", capOpaque->Env(envlen));
      }
    }

    if (repairOnClose)
    {
      //..................................................................................
      // do an upcall to the MGM and ask to adjust the replica of the uploaded file
      //..................................................................................
      XrdOucString OpaqueString = "/?mgm.pcmd=adjustreplica&mgm.path=";
      OpaqueString += capOpaque->Get("mgm.path");
      eos_info("info=\"repair on close\" path=%s", capOpaque->Get("mgm.path"));

      if (gOFS.CallManager(&error, capOpaque->Get("mgm.path"), capOpaque->Get("mgm.manager"), OpaqueString))
      {
        eos_warning("failed to execute 'adjustreplica' for path=%s", capOpaque->Get("mgm.path"));
        gOFS.Emsg(epname, error, EIO, "create all replicas - uploaded file is at risk - only one replica has been successfully stored for fn=", capOpaque->Get("mgm.path"));
      }
      else
      {
        if (!brc)
        {
          //..................................................................................
          // reset the return code
          //..................................................................................
          rc = 0;
          //..................................................................................
          // clean error message
          //..................................................................................
          gOFS.Emsg(epname, error, 0, "no error");
        }
      }

      eos_warning("executed 'adjustreplica' for path=%s - file is at low risk due to missing replica's",
                  capOpaque->Get("mgm.path"));
    }
  }
  eos_info("Return code rc=%i.", rc);
  return rc;
}


//------------------------------------------------------------------------------
// Low level read function
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readofs (XrdSfsFileOffset fileOffset,
                        char* buffer,
                        XrdSfsXferSize buffer_size)
{
  gettimeofday(&cTime, &tz);
  rCalls++;

  int rc = XrdOfsFile::read(fileOffset, buffer, buffer_size);
  eos_debug("read %llu %llu %i rc=%d", this, fileOffset, buffer_size, rc);

  if (gOFS.Simulate_IO_read_error)
  {
    return gOFS.Emsg("readofs", error, EIO, "read file - simulated IO error fn=",
                     capOpaque ? (capOpaque->Get("mgm.path") ? capOpaque->Get("mgm.path") : FName()) : FName());
  }

  // Account seeks for monitoring
  if (rOffset != static_cast<unsigned long long> (fileOffset))
  {
    if (rOffset < static_cast<unsigned long long> (fileOffset))
    {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - rOffset);
    }
    else
    {
      nBwdSeeks++;
      sBwdBytes += (rOffset - fileOffset);
    }
    if ((rOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long> (fileOffset)))
    {
      sXlFwdBytes += (fileOffset - rOffset);
      nXlFwdSeeks++;
    }
    if ((static_cast<unsigned long long> (rOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (rOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long> (fileOffset)))
    {
      sXlBwdBytes += (rOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0)
  {
    XrdSysMutexHelper vecLock(vecMutex);
    rvec.push_back(rc);
    rOffset = fileOffset + rc;
  }

  gettimeofday(&lrTime, &tz);
  AddReadTime();
  return rc;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read (XrdSfsFileOffset fileOffset,
                     XrdSfsXferSize amount)
{
  //  EPNAME("read");
  int rc = XrdOfsFile::read(fileOffset, amount);
  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset, amount);
  return rc;
}


//------------------------------------------------------------------------------
// OFS layer read entry point
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::read (XrdSfsFileOffset fileOffset,
                     char* buffer,
                     XrdSfsXferSize buffer_size)
{
  eos_debug("fileOffset=%lli, buffer_size=%i", fileOffset, buffer_size);

  if (tpcFlag == kTpcSrcRead)
  {
    if (!(rCalls % 10))
    {
      // for TPC reads we check every 10th read call if the TPC has been
      // interrupted from the client e.g. the TPC KEY has been deleted
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      if (!TpcValid())
      {
        eos_err("msg=\"tcp interrupted by control-c - cancel tcp read\" key=%s", TpcKey.c_str());
        return gOFS.Emsg("read", error, EINTR, "read - tpc transfer interrupted by client disconnect", FName());
      }
    }
  }
  
  int rc = layOut->Read(fileOffset, buffer, buffer_size);

  eos_debug("layout read %d checkSum %d", rc, checkSum);

  if ((rc > 0) && (checkSum))
  {
    XrdSysMutexHelper cLock(ChecksumMutex);
    checkSum->Add(buffer,
                  static_cast<size_t> (rc),
                  static_cast<off_t> (fileOffset));
  }

  if (rc < 0)
  {
    // Here we might take some other action
    int envlen = 0;
    eos_crit("block-read error=%d offset=%llu len=%llu file=%s",
             error.getErrInfo(),
             static_cast<unsigned long long> (fileOffset),
             static_cast<unsigned long long> (buffer_size),
             FName(),
             capOpaque ? capOpaque->Env(envlen) : FName());
    hasReadError = true; // this is used to understand if a reconstruction of a RAIN file worked
  }

  eos_debug("rc=%d offset=%lu size=%llu", rc, fileOffset,
            static_cast<unsigned long long> (buffer_size));

  if ((fileOffset + buffer_size) >= openSize)
  {
    if (checkSum)
    {
      if (!checkSum->NeedsRecalculation())
      {
        // If this is the last read of sequential reading, we can verify the checksum now
        if (verifychecksum())
          return gOFS.Emsg("read", error, EIO, "read file - wrong file checksum fn=", FName());
      }
    }
  }

  return rc;
}


//------------------------------------------------------------------------------
// Vector read - low level ofs method which is called from one of the
// layout plugins
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readvofs(XrdOucIOVec* readV,
                        uint32_t readCount)
{
  eos_debug("read count=%i", readCount);
  gettimeofday(&cTime, &tz);
  XrdSfsXferSize sz = XrdOfsFile::readv(readV, readCount);
  gettimeofday(&lrvTime, &tz);
  AddReadVTime();

  // Collect monitoring info
  {
    XrdSysMutexHelper scope_lock(vecMutex);

    for (uint32_t i = 0; i < readCount; ++i)
      monReadSingleBytes.push_back(readV[i].size);
   
    monReadvBytes.push_back(sz);
    monReadvCount.push_back(readCount);
  }

  return sz;
}

  
//------------------------------------------------------------------------------
// Vector read - OFS interface method
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::readv(XrdOucIOVec* readV,
                     int readCount)
{
  eos_debug("read count=%i", readCount);
    
  // Copy the XrdOucIOVec structure to XrdCl::ChunkList
  uint32_t total_read = 0;
  XrdCl::ChunkList chunkList;
  chunkList.reserve(readCount);

  for (int i = 0; i < readCount; ++i)
  {
    total_read += (uint32_t)readV[i].size;
    chunkList.push_back(XrdCl::ChunkInfo((uint64_t)readV[i].offset,
                                         (uint32_t)readV[i].size,
                                         (void*)readV[i].data));
  }
  
  return layOut->ReadV(chunkList, total_read);
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
XrdFstOfsFile::read (XrdSfsAio * aioparm)
{
  return SFS_ERROR;
}


//------------------------------------------------------------------------------
// Low level write function
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::writeofs (XrdSfsFileOffset fileOffset,
                         const char* buffer,
                         XrdSfsXferSize buffer_size)
{
  if (gOFS.Simulate_IO_write_error)
  {
    writeErrorFlag = kOfsSimulatedIoError;
    return gOFS.Emsg("writeofs", error, EIO, "write file - simulated IO error fn=",
                     capOpaque ? (capOpaque->Get("mgm.path") ?
                                  capOpaque->Get("mgm.path") : FName()) : FName());
  }

  if (fsid)
  {
    if (targetsize && (targetsize == bookingsize))
    {
      // Space has been successfully pre-allocated, let client write
    }
    else
    {
      // Check if the file system is full
      XrdSysMutexHelper(gOFS.Storage->fileSystemFullMapMutex);

      if (gOFS.Storage->fileSystemFullMap[fsid])
      {
        writeErrorFlag = kOfsDiskFullError;
        return gOFS.Emsg("writeofs", error, ENOSPC, "write file - disk space (headroom) exceeded fn=",
                         capOpaque ? (capOpaque->Get("mgm.path") ?
                                      capOpaque->Get("mgm.path") : FName()) : FName());
      }
    }
  }

  if (maxsize)
  {
    // Check that the user didn't exceed the maximum file size policy
    if ((fileOffset + buffer_size) > maxsize)
    {
      writeErrorFlag = kOfsMaxSizeError;
      return gOFS.Emsg("writeofs", error, ENOSPC, "write file - your file exceeds"
                       " the maximum file size setting of bytes<=",
                       capOpaque ? (capOpaque->Get("mgm.maxsize") ?
                                    capOpaque->Get("mgm.maxsize") : "<undef>") : "undef");
    }
  }

  gettimeofday(&cTime, &tz);
  wCalls++;
  
  int rc = XrdOfsFile::write(fileOffset, buffer, buffer_size);

  if (rc != buffer_size)
  {
    // Tag an io error
    writeErrorFlag = kOfsIoError;
  };

  // Account seeks for monitoring
  if (wOffset != static_cast<unsigned long long> (fileOffset))
  {
    if (wOffset < static_cast<unsigned long long> (fileOffset))
    {
      nFwdSeeks++;
      sFwdBytes += (fileOffset - wOffset);
    }
    else
    {
      nBwdSeeks++;
      sBwdBytes += (wOffset - fileOffset);
    }
    if ((wOffset + (EOS_FSTOFS_LARGE_SEEKS)) < (static_cast<unsigned long long> (fileOffset)))
    {
      sXlFwdBytes += (fileOffset - wOffset);
      nXlFwdSeeks++;
    }
    if ((static_cast<unsigned long long> (wOffset) > (EOS_FSTOFS_LARGE_SEEKS)) &&
        (wOffset - (EOS_FSTOFS_LARGE_SEEKS)) > (static_cast<unsigned long long> (fileOffset)))
    {
      sXlBwdBytes += (wOffset - fileOffset);
      nXlBwdSeeks++;
    }
  }

  if (rc > 0)
  {
    XrdSysMutexHelper(vecMutex);
    wvec.push_back(rc);
    wOffset = fileOffset + rc;    
  }

  gettimeofday(&lwTime, &tz);
  AddWriteTime();
  return rc;
}


//------------------------------------------------------------------------------
// OFS layer write entry point
//------------------------------------------------------------------------------
XrdSfsXferSize
XrdFstOfsFile::write (XrdSfsFileOffset fileOffset,
                      const char* buffer,
                      XrdSfsXferSize buffer_size)
{
  int rc = layOut->Write(fileOffset, const_cast<char*> (buffer), buffer_size);

  if ((rc < 0) && isCreation && (error.getErrInfo() == EREMOTEIO))
  {
    if (eos::common::LayoutId::GetLayoutType(lid) == eos::common::LayoutId::kReplica)
    {
      // If we see a remote IO error, we don't fail,
      // we just call a repair action afterwards (only for replica layouts!)
      repairOnClose = true;
      rc = buffer_size;
    }
  }

  // evt. add checksum
  if ((rc > 0) && (checkSum))
  {
    XrdSysMutexHelper cLock(ChecksumMutex);
    checkSum->Add(buffer,
                  static_cast<size_t> (rc),
                  static_cast<off_t> (fileOffset));
  }

  if (rc > 0)
  {
    if (static_cast<unsigned long long> (fileOffset + buffer_size) >
        static_cast<unsigned long long> (maxOffsetWritten))
      maxOffsetWritten = (fileOffset + buffer_size);
  }

  haswrite = true;
  eos_debug("rc=%d offset=%lu size=%lu", rc, fileOffset,
            static_cast<unsigned long> (buffer_size));

  /* THIS SEEMS REDUNDANT ?!
  if (rc < 0)
  {
    int envlen = 0;
    eos_crit("block-write error=%d offset=%llu len=%llu file=%s",
             error.getErrInfo(),
             static_cast<unsigned long long> (fileOffset),
             static_cast<unsigned long long> (buffer_size),
             FName(),
             capOpaque ? capOpaque->Env(envlen) : FName());
  }
  */

  if (rc < 0)
  {
    int envlen = 0;
    // Set the deletion flag for write errors
    writeDelete = true;
    XrdOucString errdetail;

    if (isCreation)
    {
      XrdOucString newerr;
      // Add to the error message that this file has been removed after the error,
      // which happens for creations
      newerr = error.getErrText();

      if (writeErrorFlag == kOfsSimulatedIoError)
      {
        // Simulated IO error
        errdetail += " => file has been removed because of a simulated IO error";
      }
      else
      {
        if (writeErrorFlag == kOfsDiskFullError)
        {
          // Sisk full error
          errdetail += " => file has been removed because the target filesystem  was full";
        }
        else
        {
          if (writeErrorFlag == kOfsMaxSizeError)
          {
            // Maximum file size error
            errdetail += " => file has been removed because the maximum target "
                         "filesize defined for that subtree was exceeded (maxsize=";
            char smaxsize[16];
            snprintf(smaxsize, sizeof ( smaxsize) - 1, "%llu", (unsigned long long) maxsize);
            errdetail += smaxsize;
            errdetail += " bytes)";
          }
          else
          {
            if (writeErrorFlag == kOfsIoError)
            {
              // Generic IO error
              errdetail += " => file has been removed due to an IO error on the target filesystem";
            }
            else
            {
              errdetail += " => file has been removed due to an IO error (unspecified)";
            }
          }
        }
      }

      newerr += errdetail.c_str();
      error.setErrInfo(error.getErrInfo(), newerr.c_str());
    }

    eos_err("block-write error=%d offset=%llu len=%llu file=%s error=\"%s\"",
            error.getErrInfo(),
            (unsigned long long) fileOffset,
            (unsigned long long) buffer_size, FName(),
            capOpaque ? capOpaque->Env(envlen) : FName(),
            errdetail.c_str());
  }

  return rc;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::write (XrdSfsAio * aioparm)
{
  return SFS_ERROR;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::syncofs ()
{
  return XrdOfsFile::sync();
}



//------------------------------------------------------------------------------
// Verify if a TPC key is still valid
//------------------------------------------------------------------------------

bool
XrdFstOfsFile::TpcValid ()
{
  // This call requires to have a lock like 
  // 'XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex)'
  if (TpcKey.length())
  {
    if (gOFS.TpcMap[isRW].count(TpcKey.c_str()))
    {
      return true;
    }
  }
  return false;
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::sync ()
{
  if (tpcFlag == kTpcDstSetup)
  {
    if (tpcState == kTpcIdle)
    {
      eos_info("msg=\"tpc enabled - 1st sync\"");
      tpcState = kTpcEnabled;
      return SFS_OK;
    }

    if (tpcState == kTpcRun)
    {
      eos_info("msg=\"tpc already running - >2nd sync\"");
      return SFS_OK;
    }

    if (tpcState == kTpcDone)
    {
      eos_info("msg=\"tpc already finisehd - >2nd sync\"");
      return SFS_OK;
    }

    if (tpcState == kTpcEnabled)
    {
      tpcState = kTpcRun;
    }

    eos_info("msg=\"tpc now running - 2nd sync\"");
    std::string src_url = "";
    std::string src_cgi = "";
    {
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      //...........................................................................
      // The sync initiates the third party copy 
      //...........................................................................
      if (!TpcValid())
      {
        tpcState = kTpcDone;
        eos_err("msg=\"tpc session invalidated during sync\"");
        error.setErrInfo(ECONNABORTED, "sync - TPC session has been closed by disconnect");
        return SFS_ERROR;
      }
      // construct the source URL
      src_url = "root://";
      src_url += gOFS.TpcMap[isRW][TpcKey.c_str()].src;
      src_url += "/";
      src_url += gOFS.TpcMap[isRW][TpcKey.c_str()].lfn;

      // construct the source CGI
      src_cgi = "tpc.key=";
      src_cgi += TpcKey.c_str();
      src_cgi += "&tpc.org=";
      src_cgi += gOFS.TpcMap[isRW][TpcKey.c_str()].org;
    }

    XrdIo tpcIO; // the remote IO object

    eos_info("sync-url=%s sync-cgi=%s\n", src_url.c_str(), src_cgi.c_str());
    if (tpcIO.Open(src_url.c_str(), 0, 0, src_cgi.c_str(), 10))
    {
      XrdOucString msg = "sync - TPC open failed for url=";
      msg += src_url.c_str();
      msg += " cgi=";
      msg += src_cgi.c_str();
      error.setErrInfo(EFAULT, msg.c_str());
      tpcState = kTpcDone;
      return SFS_ERROR;
    }

    {
      //.........................................................................
      // Re-Check validity of the TPC key
      //.........................................................................
      XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
      if (!TpcValid())
      {
        eos_err("msg=\"tpc session invalidated during sync\"");
        tpcState = kTpcDone;
        error.setErrInfo(ECONNABORTED,
                         "sync - TPC session has been closed by disconnect");
        return SFS_ERROR;
      }
    }

    int64_t rbytes = 0;
    int64_t wbytes = 0;
    off_t offset = 0;
    auto_ptr < std::vector<char> > buffer(new std::vector<char>(ReadaheadBlock::sDefaultBlocksize));

    eos_info("msg=\"tpc pull\" ");

    do
    {
      //.........................................................................
      // Read the remote file in chunks and check after each chunk if the TPC
      // has been aborted already
      //.........................................................................
      rbytes = tpcIO.Read(offset, &((*buffer)[0]), ReadaheadBlock::sDefaultBlocksize, 30);
      eos_debug("msg=\"tpc read\" rbytes=%llu request=%llu", rbytes, ReadaheadBlock::sDefaultBlocksize);
      if (rbytes == -1)
      {
        tpcState = kTpcDone;
        eos_err("msg=\"tpc transfer terminated - remote read failed\"");
        error.setErrInfo(EIO,
                         "sync - tpc remote read failed");
        return SFS_ERROR;
      }

      if (rbytes > 0)
      {
        //.......................................................................
        // Write the buffer out through the local object
        //.......................................................................
        wbytes = write(offset, &((*buffer)[0]), rbytes);
        eos_debug("msg=\"tpc write\" wbytes=%llu", wbytes);
        if (rbytes != wbytes)
        {
          tpcState = kTpcDone;
          eos_err("msg=\"tpc transfer terminated - local write failed\"");
          error.setErrInfo(EIO,
                           "sync - tpc local write failed");
          return SFS_ERROR;
        }
        offset += rbytes;
      }

      {
        //.......................................................................
        // Re-Check validity of the TPC key
        //.......................................................................
        XrdSysMutexHelper tpcLock(gOFS.TpcMapMutex);
        if (!TpcValid())
        {
          tpcState = kTpcDone;
          eos_err("msg=\"tpc transfer invalidated during sync\"");
          error.setErrInfo(ECONNABORTED,
                           "sync - TPC session has been closed by disconnect");
          return SFS_ERROR;
        }
      }
    }
    while (rbytes > 0);

    // close the remote file, if no error close the local file
    return (tpcIO.Close() || close());
  }
  else
  {
    //...........................................................................
    // Standard file sync
    //...........................................................................
    return layOut->Sync();
  }
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::sync (XrdSfsAio * aiop)
{
  return layOut->Sync();
}



//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncateofs (XrdSfsFileOffset fileOffset)
{
  // truncation moves the max offset written
  eos_debug("value=%llu", (unsigned long long) fileOffset);
  maxOffsetWritten = fileOffset;

  struct stat buf;
  // stat the current file size
  if (!::stat(fstPath.c_str(), &buf))
  {
    // if the file has the proper size we don't truncate
    if (buf.st_size == fileOffset)
      return SFS_OK;
  }
  return XrdOfsFile::truncate(fileOffset);
}



//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
int
XrdFstOfsFile::truncate (XrdSfsFileOffset fileOffset)
{
  eos_info("openSize=%llu fileOffset=%llu ", openSize, fileOffset);

  if (fileOffset != openSize)
  {
    haswrite = true;

    if (checkSum)
    {
      if (fileOffset != checkSum->GetMaxOffset())
      {
        checkSum->Reset();
        checkSum->SetDirty();
      }
    }
  }

  return layOut->Truncate(fileOffset);
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------

int
XrdFstOfsFile::stat (struct stat * buf)
{
  EPNAME("stat");
  int rc = SFS_OK;

  if (layOut)
  {
    if ((rc = layOut->Stat(buf)))
    {
      rc = gOFS.Emsg(epname, error, EIO, "stat - cannot stat layout to determine file size ", Path.c_str());
    }
  }
  else
  {
    rc = gOFS.Emsg(epname, error, ENXIO, "stat - no layout to determine file size ", Path.c_str());
  }

  // store the file id as inode number
  if (!rc)
    buf->st_ino = fileid << 28;


  eos_notice("path=%s inode=%lu", Path.c_str(), fileid);
  return rc;
}



//------------------------------------------------------------------------------
// Execute command on an open file object (version 1)
//------------------------------------------------------------------------------
int
XrdFstOfsFile::fctl(const int cmd,
                    int alen,
                    const char* args,
                    const XrdSecEntity* client)
{
  eos_debug("cmd=%i, args=%s", cmd, args);
  
  if (cmd == SFS_FCTL_SPEC1)
  {
    if (strncmp(args, "delete", alen) == 0)
    {
      eos_warning("setting deletion flag for file %s", fstPath.c_str());
      // This indicates to delete the file during the close operation
      viaDelete = true;
      return SFS_OK;
    }
  }
  
  error.setErrInfo(ENOTSUP, "fctl command not supported");
  return SFS_ERROR;
}


//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
std::string
XrdFstOfsFile::GetFstPath ()
{
  std::string ret = fstPath.c_str();
  return ret;
}


EOSFSTNAMESPACE_END

