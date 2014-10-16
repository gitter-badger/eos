// ----------------------------------------------------------------------
// File: FmdSqlite.hh
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
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/**
 * @file   FmdSqlite.hh
 * 
 * @brief  Classes for FST File Meta Data Handling.
 * 
 * 
 */

#ifndef __EOSFST_FmdSQLITE_HH__
#define __EOSFST_FmdSQLITE_HH__

/*----------------------------------------------------------------------------*/
#include "fst/Namespace.hh"
#include "common/Logging.hh"
#include "common/SymKeys.hh"
#include "common/FileId.hh"
#include "common/FileSystem.hh"
#include "common/LayoutId.hh"
#include "common/sqlite/sqlite3.h"
#include "fst/FmdHandler.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/
// this is needed because of some openssl definition conflict!
#undef des_set_key
#include <google/dense_hash_map>
#include <google/sparse_hash_map>
#include <google/sparsehash/densehashtable.h>
#include <sys/time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <zlib.h>
#include <openssl/sha.h>

#ifdef __APPLE__
#define ECOMM 70
#endif

EOSFSTNAMESPACE_BEGIN


// ---------------------------------------------------------------------------
//! Class handling many Fmd changelog files at a time
// ---------------------------------------------------------------------------

class FmdSqliteHandler : public FmdHandler
{
public:
  typedef std::vector<std::map< std::string, XrdOucString > > qr_result_t;

  struct FsCallBackInfo
  {
    eos::common::FileSystem::fsid_t fsid;
    google::dense_hash_map<unsigned long long, struct Fmd >* fmdmap;
  };

  typedef struct FsCallBackInfo fs_callback_info_t;

  std::map<eos::common::FileSystem::fsid_t, sqlite3*> *
  GetDB ()
  {
    return &DB;
  }

  // ---------------------------------------------------------------------------
  //! Return's the syncing flag (if we sync, all files on disk are flagge as orphans until the MGM meta data has been verified and when this flag is set, we don't report orphans!
  // ---------------------------------------------------------------------------

  virtual bool
  IsSyncing (eos::common::FileSystem::fsid_t fsid)
  {
    return isSyncing[fsid];
  }

  // ---------------------------------------------------------------------------
  //! Return's the dirty flag indicating a non-clean shutdown
  // ---------------------------------------------------------------------------

  virtual bool
  IsDirty (eos::common::FileSystem::fsid_t fsid)
  {
    return isDirty[fsid];
  }

  // ---------------------------------------------------------------------------
  //! Set the stay dirty flag indicating a non completed bootup
  // ---------------------------------------------------------------------------

  virtual void
  StayDirty (eos::common::FileSystem::fsid_t fsid, bool dirty)
  {
    stayDirty[fsid] = dirty;
  }

  // ---------------------------------------------------------------------------
  //! Define a DB file for a filesystem id
  // ---------------------------------------------------------------------------
  virtual bool SetDBFile (const char* dbfile, int fsid, XrdOucString option = "");

  // ---------------------------------------------------------------------------
  //! Shutdown a DB for a filesystem
  // ---------------------------------------------------------------------------
  virtual bool ShutdownDB (eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Read all Fmd entries from a DB file
  // ---------------------------------------------------------------------------
  virtual bool ReadDBFile (eos::common::FileSystem::fsid_t, XrdOucString option = "");

  // ---------------------------------------------------------------------------
  //! Trim a DB file
  // ---------------------------------------------------------------------------
  virtual bool TrimDBFile (eos::common::FileSystem::fsid_t fsid, XrdOucString option = "");
  virtual bool TrimDB ();

  // the meta data handling functions

  // ---------------------------------------------------------------------------
  //! attach or create a fmd record
  // ---------------------------------------------------------------------------
  virtual FmdHelper* GetFmd (eos::common::FileId::fileid_t fid, eos::common::FileSystem::fsid_t fsid, uid_t uid, gid_t gid, eos::common::LayoutId::layoutid_t layoutid, bool isRW = false, bool force = false);

  // ---------------------------------------------------------------------------
  //! Delete an fmd record
  // ---------------------------------------------------------------------------
  virtual bool DeleteFmd (eos::common::FileId::fileid_t fid, eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Commit a modified fmd record
  // ---------------------------------------------------------------------------
  virtual bool Commit (FmdHelper* fmd, bool lockit = true);

  // ---------------------------------------------------------------------------
  //! Commit a modified fmd record without locks and change of modification time
  // ---------------------------------------------------------------------------
  virtual bool CommitFromMemory (eos::common::FileId::fileid_t fid, eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Reset Disk Information
  // ---------------------------------------------------------------------------
  virtual bool ResetDiskInformation (eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Reset Mgm Information
  // ---------------------------------------------------------------------------
  virtual bool ResetMgmInformation (eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Update fmd from disk contents
  // ---------------------------------------------------------------------------
  virtual bool UpdateFromDisk (eos::common::FileSystem::fsid_t fsid, eos::common::FileId::fileid_t fid, unsigned long long disksize, std::string diskchecksum, unsigned long checktime, bool filecxerror, bool blockcxerror, bool flaglayouterror);


  // ---------------------------------------------------------------------------
  //! Update fmd from mgm contents
  // ---------------------------------------------------------------------------
  virtual bool UpdateFromMgm (eos::common::FileSystem::fsid_t fsid, eos::common::FileId::fileid_t fid, eos::common::FileId::fileid_t cid, eos::common::LayoutId::layoutid_t lid, unsigned long long mgmsize, std::string mgmchecksum, uid_t uid, gid_t gid, unsigned long long ctime, unsigned long long ctime_ns, unsigned long long mtime, unsigned long long mtime_ns, int layouterror, std::string locations);

  // ---------------------------------------------------------------------------
  //! Resync File meta data found under path
  // ---------------------------------------------------------------------------
  virtual bool ResyncAllDisk (const char* path, eos::common::FileSystem::fsid_t fsid, bool flaglayouterror);

  // ---------------------------------------------------------------------------
  //! Resync a single entry from Disk
  // ---------------------------------------------------------------------------
  virtual bool ResyncDisk (const char* fstpath, eos::common::FileSystem::fsid_t fsid, bool flaglayouterror, bool callautorepair=false);

  // ---------------------------------------------------------------------------
  //! Resync a single entry from Mgm
  // ---------------------------------------------------------------------------
  virtual bool ResyncMgm (eos::common::FileSystem::fsid_t fsid, eos::common::FileId::fileid_t fid, const char* manager);

  // ---------------------------------------------------------------------------
  //! Resync all entries from Mgm
  // ---------------------------------------------------------------------------
  virtual bool ResyncAllMgm (eos::common::FileSystem::fsid_t fsid, const char* manager);

  // ---------------------------------------------------------------------------
  //! Query list of fids
  // ---------------------------------------------------------------------------
  size_t Query (eos::common::FileSystem::fsid_t fsid, std::string query, std::vector<eos::common::FileId::fileid_t> &fidvector);

  // ---------------------------------------------------------------------------
  //! GetIncosistencyStatistics
  // ---------------------------------------------------------------------------
  virtual bool GetInconsistencyStatistics (eos::common::FileSystem::fsid_t fsid, std::map<std::string, size_t> &statistics, std::map<std::string, std::set < eos::common::FileId::fileid_t> > &fidset);

  // ---------------------------------------------------------------------------
  //! Initialize the changelog hash
  // ---------------------------------------------------------------------------

  virtual void
  Reset (eos::common::FileSystem::fsid_t fsid)
  {
    // you need to lock the RWMutex Mutex before calling this
    FmdMap[fsid].clear();
  }

  // ---------------------------------------------------------------------------
  //! Hash map pointing from fid to offset in changelog file
  // ---------------------------------------------------------------------------
  google::sparse_hash_map<eos::common::FileSystem::fsid_t, google::dense_hash_map<unsigned long long, struct Fmd > > FmdMap;

  // ---------------------------------------------------------------------------
  //! Initialize the SQL DB
  // ---------------------------------------------------------------------------
  virtual bool ResetDB (eos::common::FileSystem::fsid_t fsid);

  // ---------------------------------------------------------------------------
  //! Constructor
  // ---------------------------------------------------------------------------

  FmdSqliteHandler ()
  {
    SetLogId("CommonFmdSqliteHandler");
  }

  // ---------------------------------------------------------------------------
  //! Constructor
  // ---------------------------------------------------------------------------

  ~FmdSqliteHandler ()
  {
    Shutdown();
  }

  // ---------------------------------------------------------------------------
  //! Shutdown
  // ---------------------------------------------------------------------------

  void
  Shutdown ()
  {
    // clean-up all open DB handles
    std::map<eos::common::FileSystem::fsid_t, sqlite3*>::const_iterator it;
    for (it = DB.begin(); it != DB.end(); it++)
    {
      ShutdownDB(it->first);
    }
    {
      // remove all
      eos::common::RWMutexWriteLock lock(Mutex);
      DB.clear();
    }
  }

private:
  qr_result_t Qr;
  std::map<eos::common::FileSystem::fsid_t, sqlite3*> DB;
  std::map<eos::common::FileSystem::fsid_t, std::string> DBfilename;

  char* ErrMsg;
  static int CallBack (void * object, int argc, char **argv, char **ColName);
  static int ReadDBCallBack (void * object, int argc, char **argv, char **ColName);
};


// ---------------------------------------------------------------------------
extern FmdSqliteHandler gFmdSqliteHandler;

EOSFSTNAMESPACE_END

#endif
