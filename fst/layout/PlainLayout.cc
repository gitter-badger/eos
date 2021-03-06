//------------------------------------------------------------------------------
// File: PlainLayout.cc
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
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include "fst/layout/PlainLayout.hh"
#include "fst/io/FileIoPlugin.hh"
#include "fst/io/AsyncMetaHandler.hh"
#include "fst/XrdFstOfsFile.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
PlainLayout::PlainLayout (XrdFstOfsFile* file,
                          unsigned long lid,
                          const XrdSecEntity* client,
                          XrdOucErrInfo* outError,
                          eos::common::LayoutId::eIoType io,
                          uint16_t timeout) :
    Layout (file, lid, client, outError, io, timeout),
    mFileSize(0),
    mDisableRdAhead(false)
{
  // For the plain layout we use only the LocalFileIo type
  mPlainFile = FileIoPlugin::GetIoObject(mIoType, mOfsFile, mSecEntity);
  mIsEntryServer = true;
}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
PlainLayout::~PlainLayout ()
{
  delete mPlainFile;
}

//------------------------------------------------------------------------------
// Open File
//------------------------------------------------------------------------------
int
PlainLayout::Open (const std::string& path,
                   XrdSfsFileOpenMode flags,
                   mode_t mode,
                   const char* opaque)
{
  mLocalPath = path;
  int retc = mPlainFile->Open(path, flags, mode, opaque, mTimeout);

  if (retc)
  {
    eos_err("failed open for file=%s", path.c_str());
    return SFS_ERROR;
  }

  mLastUrl = mPlainFile->GetLastUrl();

  // Get initial file size
  struct stat st_info;
  int retc_stat = mPlainFile->Stat(&st_info);

  if (retc_stat)
  {
    eos_err("failed stat for file=%s", path.c_str());
    return SFS_ERROR;
  }

  mFileSize = st_info.st_size;
  return retc;
}

//------------------------------------------------------------------------------
// Read from file
//------------------------------------------------------------------------------
int64_t
PlainLayout::Read (XrdSfsFileOffset offset, char* buffer,
                   XrdSfsXferSize length, bool readahead)
{
  if (readahead && !mDisableRdAhead)
  {
    if (mIoType == eos::common::LayoutId::eIoType::kXrdCl)
    {
      if ((uint64_t)(offset + length) > mFileSize)
	length = mFileSize - offset;

      if (length<0)
	length = 0;

      eos_static_info("read offset=%llu length=%lu", offset, length);
      int64_t nread = mPlainFile->ReadAsync(offset, buffer, length, readahead);

      // Wait for any async requests
      AsyncMetaHandler* ptr_handler = static_cast<AsyncMetaHandler*>
          (mPlainFile->GetAsyncHandler());

      if (ptr_handler)
      {
        uint16_t error_type = ptr_handler->WaitOK();

        if (error_type != XrdCl::errNone)
          return SFS_ERROR;
      }

      if ( (nread+offset) > (off_t)mFileSize)
	mFileSize = nread+offset;

      if ( (nread != length) && ( (nread+offset) < (int64_t)mFileSize) )
	mFileSize = nread+offset;

      return nread;
    }
  }

  return mPlainFile->Read(offset, buffer, length, mTimeout);
}

//------------------------------------------------------------------------------
// Vector read 
//------------------------------------------------------------------------------
int64_t
PlainLayout::ReadV (XrdCl::ChunkList& chunkList, uint32_t len)
{
  return mPlainFile->ReadV(chunkList);
}


//------------------------------------------------------------------------------
// Write to file
//------------------------------------------------------------------------------
int64_t
PlainLayout::Write (XrdSfsFileOffset offset, const char* buffer,
                    XrdSfsXferSize length)
{
  mDisableRdAhead = true;

  if ((uint64_t)(offset + length) > mFileSize)
    mFileSize = offset + length;

  return mPlainFile->Write(offset, buffer, length, mTimeout);
}

//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------
int
PlainLayout::Truncate (XrdSfsFileOffset offset)
{
  mFileSize = offset;
  return mPlainFile->Truncate(offset, mTimeout);
}

//------------------------------------------------------------------------------
// Reserve space for file
//------------------------------------------------------------------------------
int
PlainLayout::Fallocate (XrdSfsFileOffset length)
{
  return mPlainFile->Fallocate(length);
}

//------------------------------------------------------------------------------
// Deallocate reserved space
//------------------------------------------------------------------------------
int
PlainLayout::Fdeallocate (XrdSfsFileOffset fromOffset, XrdSfsFileOffset toOffset)
{
  return mPlainFile->Fdeallocate(fromOffset, toOffset);
}

//------------------------------------------------------------------------------
// Sync file to disk
//------------------------------------------------------------------------------
int
PlainLayout::Sync ()
{
  return mPlainFile->Sync(mTimeout);
}

//------------------------------------------------------------------------------
// Get stats for file
//------------------------------------------------------------------------------
int
PlainLayout::Stat (struct stat* buf)
{
  return mPlainFile->Stat(buf, mTimeout);
}

//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
PlainLayout::Close ()
{
  return mPlainFile->Close(mTimeout);
}

//------------------------------------------------------------------------------
// Remove file
//------------------------------------------------------------------------------
int
PlainLayout::Remove ()
{
  return mPlainFile->Remove();
}

EOSFSTNAMESPACE_END
