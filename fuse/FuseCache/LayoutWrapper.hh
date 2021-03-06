//------------------------------------------------------------------------------
// File LayoutWrapper.hh
// Author: Geoffray Adde <geoffray.adde@cern.ch> CERN
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
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

#ifndef __EOS_FUSE_LAYOUTWRAPPER_HH__
#define __EOS_FUSE_LAYOUTWRAPPER_HH__

#include "FileAbstraction.hh"
#include "Bufferll.hh"

//------------------------------------------------------------------------------
//! Class that wraps a FileLayout to keep track of change times and to
//  be able to close the layout and be able to automatically reopen it if it's needed
//  This is a trick needed because the flush function if the fuse API can be called
//  several times and it's not clear if the file can still be used in between
//  those times.
//------------------------------------------------------------------------------
class LayoutWrapper
{
  friend class FileAbstraction;

  eos::fst::Layout* mFile;
  bool mOpen;
  std::string mPath;
  unsigned long long mInode;
  XrdSfsFileOpenMode mFlags;
  mode_t mMode;
  std::string mOpaque;
  std::string mLazyUrl;
  FileAbstraction *mFabs;
  timespec mLocalUtime[2];

  XrdSysMutex mMakeOpenMutex;

  std::shared_ptr<Bufferll> mCache;

  struct CacheEntry {
    std::shared_ptr<Bufferll> mCache;
    time_t mLifeTime;
    time_t mOwnerLifeTime;
    int64_t  mSize;
  };

  static XrdSysMutex gCacheAuthorityMutex;
  static std::map<unsigned long long, LayoutWrapper::CacheEntry> gCacheAuthority;

  bool mCanCache;
  bool mCacheCreator;
  off_t mMaxOffset;
  int64_t mSize;

  //--------------------------------------------------------------------------
  //! do the open on the mgm but not on the fst yet
  //--------------------------------------------------------------------------
  int LazyOpen (const std::string& path, XrdSfsFileOpenMode flags, mode_t mode, const char* opaque, const struct stat *buf);


public:
  //--------------------------------------------------------------------------
  //! Constructor
  //!
  //! @param file layout to be wrapped
  //!
  //--------------------------------------------------------------------------
  LayoutWrapper (eos::fst::Layout* file);

  //----------------------------------------------------------------------------
  //! Destructor
  //----------------------------------------------------------------------------
  ~LayoutWrapper ();

  //--------------------------------------------------------------------------
  //! Make sure that the file layout is open
  //! Reopen it if needed using (almost) the same argument as the previous open
  //--------------------------------------------------------------------------
  int MakeOpen ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  const char*
  GetName ();

  //--------------------------------------------------------------------------
  //! return the path of the file
  //--------------------------------------------------------------------------
  inline const char*
  GetPath () { return mPath.c_str(); }

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  const char*
  GetLocalReplicaPath ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  unsigned int
  GetLayoutId ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  const std::string&
  GetLastUrl ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  bool
  IsEntryServer ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int Open (const std::string& path, XrdSfsFileOpenMode flags, mode_t mode, const char* opaque, const struct stat *buf, bool doOpen=true, size_t creator_lifetime=30);

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int64_t CacheSize();
  int64_t Read (XrdSfsFileOffset offset, char* buffer, XrdSfsXferSize length, bool readahead = false);
  int64_t ReadCache (XrdSfsFileOffset offset, char* buffer, XrdSfsXferSize length, off_t maxcache=(64*1024*1024));

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int64_t ReadV (XrdCl::ChunkList& chunkList, uint32_t len);

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int64_t Write (XrdSfsFileOffset offset, const char* buffer, XrdSfsXferSize length, bool touchMtime=true);
  int64_t WriteCache (XrdSfsFileOffset offset, const char* buffer, XrdSfsXferSize length, off_t maxcache=(64*1024*1024));

  // size known after open if this file was created here
  int64_t Size()
  {
    return mSize;
  }
  
  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int Truncate (XrdSfsFileOffset offset, bool touchMtime=true);

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int Sync ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int Close ();

  //--------------------------------------------------------------------------
  //! overloading member functions of FileLayout class
  //--------------------------------------------------------------------------
  int Stat (struct stat* buf);

  //--------------------------------------------------------------------------
  //! Set atime and mtime according to argument without commit at file closure
  //--------------------------------------------------------------------------
  void Utimes ( const struct stat *buf);

  //--------------------------------------------------------------------------
  //! Get Last Opened Path
  //--------------------------------------------------------------------------
  std::string GetLastPath ();

  //--------------------------------------------------------------------------
  //! Is the file Opened
  //--------------------------------------------------------------------------
  bool IsOpen ();

  //--------------------------------------------------------------------------
  //! Path accessor
  //--------------------------------------------------------------------------
  inline const std::string & GetOpenPath() const {return mPath;}

  //--------------------------------------------------------------------------
  //! Open Flags accessors
  //--------------------------------------------------------------------------
  inline const XrdSfsFileOpenMode & GetOpenFlags() const {return mFlags;}

  //--------------------------------------------------------------------------
  //! Utility function to import (key,value) from a cgi string to a map
  //--------------------------------------------------------------------------
  static bool ImportCGI(std::map<std::string,std::string> &m, const std::string &cgi);

  //--------------------------------------------------------------------------
  //! Utility function to write the content of a(key,value) map to a cgi string
  //--------------------------------------------------------------------------
  static bool ToCGI(const std::map<std::string,std::string> &m , std::string &cgi);

  //--------------------------------------------------------------------------
  //! Check if we can cache ..
  //--------------------------------------------------------------------------
  bool CanCache() const {return mCanCache;}
};

#endif
