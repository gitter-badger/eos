// ----------------------------------------------------------------------
// File: Http.hh
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

#ifndef __EOSFST_HTTP__HH__
#define __EOSFST_HTTP__HH__

/*----------------------------------------------------------------------------*/
#include "fst/Namespace.hh"
#include "common/Http.hh"

/*----------------------------------------------------------------------------*/
#include "XrdSys/XrdSysPthread.hh"
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

class Http : public eos::common::Http
{
  // -------------------------------------------------------------
  // ! creates an Http redirector instance running on the FST
  // -------------------------------------------------------------

public:

  /**
   * Constructor
   */
  Http (int port = 8000);

  /**
   * Destructor
   */
  virtual ~Http ();

#ifdef EOS_MICRO_HTTPD
  /**
   * http object handler function on FST
   * @return see implementation
   */
  virtual int Handler (void *cls,
                       struct MHD_Connection *connection,
                       const char *url,
                       const char *method,
                       const char *version,
                       const char *upload_data,
                       size_t *upload_data_size, void **ptr);

  /**
   * File Read Callback function
   * @param cls XrdOfsFile* object
   * @param pos offset to read from
   * @param buf buffer to write to
   * @param max size to read
   * @return number of bytes read
   */
  static ssize_t FileReaderCallback (void *cls, uint64_t pos, char *buf, size_t max);

  /**
   * File Close Callback function
   * @param cls XrdOfsFile* object
   */
  static void FileCloseCallback (void *cls);
#endif
};

EOSFSTNAMESPACE_END

#endif
