//------------------------------------------------------------------------------
// File: ReplicaParLayout.cc
// Author: Andreas-Joachim Peters - CERN
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
#include "fst/layout/ReplicaParLayout.hh"
#include "fst/io/FileIoPlugin.hh"
#include "fst/io/AsyncMetaHandler.hh"
#include "fst/XrdFstOfs.hh"

/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN


//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
ReplicaParLayout::ReplicaParLayout (XrdFstOfsFile* file,
                                    unsigned long lid,
                                    const XrdSecEntity* client,
                                    XrdOucErrInfo* outError,
                                    eos::common::LayoutId::eIoType io,
                                    uint16_t timeout) :
Layout (file, lid, client, outError, io, timeout)
{
  mNumReplicas = eos::common::LayoutId::GetStripeNumber(lid) + 1; // this 1=0x0 16=0xf :-)
  ioLocal = false;
}


//------------------------------------------------------------------------------
// Open file
//------------------------------------------------------------------------------
int
ReplicaParLayout::Open (const std::string& path,
                        XrdSfsFileOpenMode flags,
                        mode_t mode,
                        const char* opaque)
{
 // No replica index definition indicates that this is gateway access just
 // forwarding to another remote server
 int replica_index = -1;
 int replica_head = -1;
 bool is_gateway = false;
 bool is_head_server = false;
 const char* index = mOfsFile->openOpaque->Get("mgm.replicaindex");

 if (index)
 {
   replica_index = atoi(index);

   if ((replica_index < 0) ||
       (replica_index > eos::common::LayoutId::kSixteenStripe))
   {
     eos_err("illegal replica index %d", replica_index);
     return gOFS.Emsg("ReplicaPar::Open", *mError, EINVAL,
                      "open replica - illegal replica index found", index);
   }

   ioLocal = true;
 }
 else
 {
   ioLocal = false;
   is_gateway = true;
 }

 const char* head = mOfsFile->openOpaque->Get("mgm.replicahead");

 if (head)
 {
   replica_head = atoi(head);

   if ((replica_head < 0) ||
       (replica_head > eos::common::LayoutId::kSixteenStripe))
   {
     eos_err("illegal replica head %d", replica_head);
     return gOFS.Emsg("ReplicaParOpen", *mError, EINVAL,
                      "open replica - illegal replica head found", head);
   }
 }
 else
 {
   eos_err("replica head missing");
   return gOFS.Emsg("ReplicaPar::Open", *mError, EINVAL,
                    "open replica - no replica head defined");
 }

 // Define the replication head
 eos_info("replica_head=%i, replica_index = %i.", replica_head, replica_index);

 if (replica_index == replica_head)
 {
   is_head_server = true;
 }

 // Define if this is the first client contact point
 if (is_gateway || (!is_gateway && is_head_server))
 {
   mIsEntryServer = true;
 }

 int envlen;
 XrdOucString remoteOpenOpaque = mOfsFile->openOpaque->Env(envlen);
 XrdOucString remoteOpenPath = mOfsFile->openOpaque->Get("mgm.path");

 // Only a gateway or head server needs to contact others
 if (is_gateway || is_head_server)
 {
   // Assign stripe URLs
   std::string replica_url;

   for (int i = 0; i < mNumReplicas; i++)
   {
     XrdOucString reptag = "mgm.url";
     reptag += i;
     const char* rep = mOfsFile->capOpaque->Get(reptag.c_str());

     if (!rep)
     {
       eos_err("Failed to open replica - missing url for replica %s",
               reptag.c_str());
       return gOFS.Emsg("ReplicaParOpen", *mError, EINVAL,
                        "open stripes - missing url for replica ",
                        reptag.c_str());
     }

     // Check if the first replica is remote
     replica_url = rep;
     replica_url += remoteOpenPath.c_str();
     replica_url += "?";

     // Prepare the index for the next target
     remoteOpenOpaque = mOfsFile->openOpaque->Env(envlen);

     if (index)
     {
       XrdOucString oldindex = "mgm.replicaindex=";
       XrdOucString newindex = "mgm.replicaindex=";
       oldindex += index;
       newindex += i;
       remoteOpenOpaque.replace(oldindex.c_str(), newindex.c_str());
     }
     else
     {
       // This points now to the head
       remoteOpenOpaque += "&mgm.replicaindex=";
       remoteOpenOpaque += head;
     }

     replica_url += remoteOpenOpaque.c_str();
     mReplicaUrl.push_back(replica_url);
     eos_debug("added replica_url = %s, index = %i", replica_url.c_str(), i);
   }
 }

 // Open all the replicas needed
 for (int i = 0; i < mNumReplicas; i++)
 {
   if ((ioLocal) && (i == replica_index))
   {
     // Only the referenced entry URL does local IO
     mLocalPath = path;
     mReplicaUrl.push_back(mLocalPath);
     FileIo* file = FileIoPlugin::GetIoObject(eos::common::LayoutId::kLocal,
                                              mOfsFile, mSecEntity);

     if (file->Open(path, flags, mode, opaque, mTimeout))
     {
       eos_err("Failed to open replica - local open failed on path=%s errno=%d", path.c_str(), errno);
       return gOFS.Emsg("ReplicaOpen", *mError, errno,
                        "open replica - local open failed ", path.c_str());
     }

     mLastUrl = file->GetLastUrl();

     // Local replica is always on the first position in the vector
     mReplicaFile.insert(mReplicaFile.begin(), file);
   }
   else
   {
     // Gateway contacts the head, head contacts all
     if ((is_gateway && (i == replica_head)) ||
         (is_head_server && (i != replica_index)))
     {
       if (mOfsFile->isRW)
       {
         XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
         // Mask some opaque parameters to shorten the logging
         eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
         eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
         eos::common::StringConversion::MaskTag(maskUrl, "authz");
         FileIo* file = FileIoPlugin::GetIoObject(eos::common::LayoutId::kXrdCl,
                                                  mOfsFile, mSecEntity);

         // Write case
         if (file->Open(mReplicaUrl[i], flags, mode, opaque, mTimeout))
         {
           eos_err("Failed to open stripes - remote open failed on %s",
                   maskUrl.c_str());
           return gOFS.Emsg("ReplicaParOpen", *mError, EREMOTEIO,
                            "open stripes - remote open failed ",
                            maskUrl.c_str());
         }
	 mLastUrl = file->GetLastUrl();
         mReplicaFile.push_back(file);
         eos_debug("Opened remote file for IO: %s.", maskUrl.c_str());
       }
       else
       {
         // Read case just uses one replica
         eos_debug("Read case uses just one replica.");
         continue;
       }
     }
   }
 }

 return SFS_OK;
}


//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
ReplicaParLayout::~ReplicaParLayout ()
{
  while (!mReplicaFile.empty())
  {
    FileIo* file_io = mReplicaFile.back();
    mReplicaFile.pop_back();
    delete file_io;
  }
}


//------------------------------------------------------------------------------
// Read from file
//------------------------------------------------------------------------------
int64_t
ReplicaParLayout::Read (XrdSfsFileOffset offset, char* buffer,
                        XrdSfsXferSize length, bool readahead)
{
 int64_t rc = 0;

 for (unsigned int i = 0; i < mReplicaFile.size(); i++)
 {
   rc = mReplicaFile[i]->Read(offset, buffer, length, mTimeout);

   if (rc != length)
   {
     XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
     // mask some opaque parameters to shorten the logging
     eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
     eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
     eos::common::StringConversion::MaskTag(maskUrl, "authz");
     eos_warning("Failed to read from replica off=%lld, lenght=%i, mask_url=%s",
                 offset, length, maskUrl.c_str());
     continue;
   }
   else
   {
     // Read was scucessful no need to read from another replica
     break;
   }
 }

 if (rc == SFS_ERROR)
 {
   eos_err("Failed to read from any replica offset=%lld, lenght=%i",
           offset, length);
   return gOFS.Emsg("ReplicaParRead", *mError, EREMOTEIO,
                    "read replica - read failed");
 }

 return rc;
}



//------------------------------------------------------------------------------
// Vector read 
//------------------------------------------------------------------------------
int64_t
ReplicaParLayout::ReadV (XrdCl::ChunkList& chunkList, uint32_t len) 
{
 int64_t rc = 0;
 eos_debug("read count=%i", chunkList.size());

 for (unsigned int i = 0; i < mReplicaFile.size(); i++)
 {
   rc = mReplicaFile[i]->ReadV(chunkList, mTimeout);

   if (rc == SFS_ERROR)
   {
     XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
     // Mask some opaque parameters to shorten the logging
     eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
     eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
     eos::common::StringConversion::MaskTag(maskUrl, "authz");
     eos_warning("Failed to readv from replica -%s", maskUrl.c_str());
     continue;
   }
   else
   {
     // Read was successful no need to read from another replica
     break;
   }
 }

 if (rc == SFS_ERROR)
 {
   eos_err("Failed to readv from any replica");
   return gOFS.Emsg("ReplicaParRead", *mError, EREMOTEIO, "readv replica failed");
 }

 return rc;
}


//------------------------------------------------------------------------------
// Write to file
//------------------------------------------------------------------------------
int64_t
ReplicaParLayout::Write (XrdSfsFileOffset offset,
                         const char* buffer,
                         XrdSfsXferSize length)
{
  int64_t rc;

  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    rc = mReplicaFile[i]->WriteAsync(offset, buffer, length, mTimeout);

    if (rc != length)
    {
      XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
      // mask some opaque parameters to shorten the logging
      eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
      eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
      eos::common::StringConversion::MaskTag(maskUrl, "authz");

      if (i != 0) errno = EREMOTEIO;

      eos_err("Failed to write replica %i - write failed - %llu %s",
              i, offset, maskUrl.c_str());
      return gOFS.Emsg("ReplicaWrite", *mError, errno, "write replica failed",
                       maskUrl.c_str());
    }
  }

  return length;
}


//------------------------------------------------------------------------------
// Truncate file
//------------------------------------------------------------------------------
int
ReplicaParLayout::Truncate (XrdSfsFileOffset offset)
{
  int rc = SFS_OK;

  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    rc = mReplicaFile[i]->Truncate(offset, mTimeout);

    if (rc != SFS_OK)
    {
      if (i != 0) errno = EREMOTEIO;

      XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
      // mask some opaque parameters to shorten the logging
      eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
      eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
      eos::common::StringConversion::MaskTag(maskUrl, "authz");
      eos_err("Failed to truncate replica %i", i);
      return gOFS.Emsg("ReplicaParTuncate", *mError, errno, "truncate failed",
                       maskUrl.c_str());
    }
  }

  return rc;
}


//------------------------------------------------------------------------------
// Get stats for file
//------------------------------------------------------------------------------
int
ReplicaParLayout::Stat (struct stat* buf)
{
  int rc = 0;
  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    rc = mReplicaFile[i]->Stat(buf, mTimeout);
    // we stop with the first stat which works
    if (!rc)
      break;
  }
  return rc;
}


//------------------------------------------------------------------------------
// Sync file to disk
//------------------------------------------------------------------------------
int
ReplicaParLayout::Sync ()
{
  int rc = 0;

  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    XrdOucString maskUrl = mReplicaUrl[i].c_str() ? mReplicaUrl[i].c_str() : "";
    // mask some opaque parameters to shorten the logging
    eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
    eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
    eos::common::StringConversion::MaskTag(maskUrl, "authz");
    rc = mReplicaFile[i]->Sync(mTimeout);

    if (rc != SFS_OK)
    {
      if (i != 0) errno = EREMOTEIO;

      eos_err("error=failed to sync replica %i", i);
      return gOFS.Emsg("ReplicaParSync", *mError, errno, "sync failed",
                       maskUrl.c_str());
    }
  }

  return rc;
}


//------------------------------------------------------------------------------
// Remove file and all replicas
//------------------------------------------------------------------------------
int
ReplicaParLayout::Remove ()
{
  int rc = SFS_OK;
  bool got_error = false;

  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    rc = mReplicaFile[i]->Remove();

    if (rc != SFS_OK)
    {
      XrdOucString maskUrl = mReplicaUrl[0].c_str() ? mReplicaUrl[i].c_str() : "";
      // mask some opaque parameters to shorten the logging
      eos::common::StringConversion::MaskTag(maskUrl, "cap.sym");
      eos::common::StringConversion::MaskTag(maskUrl, "cap.msg");
      eos::common::StringConversion::MaskTag(maskUrl, "authz");
      got_error = true;

      if (i != 0) errno = EREMOTEIO;

      eos_err("error=failed to remove replica %i", i);
    }
  }

  if (got_error)
  {
    return gOFS.Emsg("ReplicaParRemove", *mError, errno, "remove failed");
  }

  return rc;
}


//------------------------------------------------------------------------------
// Close file
//------------------------------------------------------------------------------
int
ReplicaParLayout::Close ()
{
  int rc = SFS_OK;
  int rc_close = SFS_OK;

  for (unsigned int i = 0; i < mReplicaFile.size(); i++)
  {
    // Wait for any async requests before closing
    if (mReplicaFile[i])
    {
      AsyncMetaHandler* ptr_handler =
              static_cast<AsyncMetaHandler*> (mReplicaFile[i]->GetAsyncHandler());

      if (ptr_handler)
      {
        if (ptr_handler->WaitOK() != XrdCl::errNone)
        {
          eos_err("error=async requests failed for replica %s", mReplicaUrl[i].c_str());
          rc = SFS_ERROR;
        }
      }
    }

    rc_close = mReplicaFile[i]->Close(mTimeout);
    rc += rc_close;

    if (rc_close != SFS_OK)
    {
      if (i != 0) errno = EREMOTEIO;
      eos_err("error=failed to close replica %s", mReplicaUrl[i].c_str());
    }
  }

  if (rc != SFS_OK)
  {
    return gOFS.Emsg("ReplicaParClose", *mError, errno, "close failed", "");
  }

  return rc;
}


//------------------------------------------------------------------------------
// Reserve space for file
//------------------------------------------------------------------------------
int
ReplicaParLayout::Fallocate (XrdSfsFileOffset length)
{
  return mReplicaFile[0]->Fallocate(length);
}


//------------------------------------------------------------------------------
// Deallocate reserved space
//------------------------------------------------------------------------------
int
ReplicaParLayout::Fdeallocate (XrdSfsFileOffset fromOffset,
                               XrdSfsFileOffset toOffset)
{
  return mReplicaFile[0]->Fdeallocate(fromOffset, toOffset);
}


EOSFSTNAMESPACE_END

