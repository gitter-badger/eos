// ----------------------------------------------------------------------
// File: Path.hh
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

#ifndef __EOSCOMMON_PATH__
#define __EOSCOMMON_PATH__

/*----------------------------------------------------------------------------*/
#include "common/Namespace.hh"
/*----------------------------------------------------------------------------*/
#include "XrdOuc/XrdOucString.hh"
/*----------------------------------------------------------------------------*/
#include <vector>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
/*----------------------------------------------------------------------------*/

EOSCOMMONNAMESPACE_BEGIN

class Path {
private:
  XrdOucString fullPath;
  XrdOucString parentPath;
  XrdOucString lastPath;
  std::vector<std::string> subPath;

public:
  const char* GetName()                  { return lastPath.c_str();}
  const char* GetPath()                  { return fullPath.c_str();}
  const char* GetParentPath()            { return parentPath.c_str();    }
  const char* GetSubPath(unsigned int i) { if (i<subPath.size()) return subPath[i].c_str(); else return 0; }
  unsigned int GetSubPathSize()          { return subPath.size();  }

  Path(const char* path){
    fullPath = path;

    if (fullPath.endswith('/')) 
      fullPath.erase(fullPath.length()-1);
    
    // remove  /.$
    if (fullPath.endswith("/.")) {
      fullPath.erase(fullPath.length()-2);
    }

    // reassign /..
    if (fullPath.endswith("/..")) {
      fullPath.erase(fullPath.length()-3);
      int spos = fullPath.rfind("/");
      if (spos != STR_NPOS) {
        fullPath.erase(spos);
      }
    }

    int lastpos=0;
    int pos=0;
    do {
      pos = fullPath.find("/",pos);
      std::string subpath;
      if (pos!=STR_NPOS) {
        subpath.assign(fullPath.c_str(),pos+1);
        subPath.push_back(subpath);
        lastpos = pos;
        pos++;
      }
    } while (pos!=STR_NPOS);
    parentPath.assign(fullPath,0,lastpos);
    lastPath.assign(fullPath,lastpos+1);
  }


  bool MakeParentPath(mode_t mode) {
    int retc=0;
    struct stat buf;
        
    if (stat(GetParentPath(),&buf)) {
      for (int i=GetSubPathSize(); i>=0; i--) {
        // go backwards until the directory exists
        if (!stat(GetSubPath(i), &buf)) {
          // this exists
          for (int j=i+1 ;  j < (int)GetSubPathSize(); j++) {
            retc |= mkdir(GetSubPath(j), mode);
          }
          break;
        }
      }
    }
    
    if (retc)
      return false;
    return true;
  }
  
  ~Path(){};
};

EOSCOMMONNAMESPACE_END

#endif

