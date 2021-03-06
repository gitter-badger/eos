// ----------------------------------------------------------------------
// File: com_fuse.cc
// Author: Andreas-Joachim Peters - CERN
// ----------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright(C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 *(at your option) any later version.                                  *
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
#include "console/ConsoleMain.hh"
/*----------------------------------------------------------------------------*/

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

extern XrdOucString serveruri;

/* mount/umount via fuse */
int
com_fuse (char* arg1)
{
 if (interactive)
 {
   fprintf(stderr, "error: don't call <fuse> from an interactive shell - call via 'eos fuse ...'!\n");
   global_retc = -1;
   return 0;
 }

 // split subcommands
 XrdOucString mountpoint = "";
 eos::common::StringTokenizer subtokenizer(arg1);
 subtokenizer.GetLine();
 XrdOucString cmd = subtokenizer.GetToken();
 XrdOucString option = "";
 XrdOucString logfile = "";
 XrdOucString fsname = serveruri;
 fsname.replace("root://", "");
 XrdOucString params = "max_readahead=131072,max_write=4194304,fsname=";
 params += fsname;

 if (wants_help(arg1))
   goto com_fuse_usage;

 if ((cmd != "mount") &&(cmd != "umount"))
   goto com_fuse_usage;

 do
 {
   option = subtokenizer.GetToken();
   if (!option.length())
     break;
   if (option.beginswith("-o"))
   {
     params = subtokenizer.GetToken();
     if (!params.length())
       goto com_fuse_usage;
   }
   else
   {
     if (option.beginswith("-l"))
     {
       logfile = subtokenizer.GetToken();
       if (!logfile.length())
         goto com_fuse_usage;
     }
     else
     {
       break;
     }
   }
 }
 while (1);

 mountpoint = option;
 if (!mountpoint.length())
   goto com_fuse_usage;

 if (mountpoint.beginswith("-"))
   goto com_fuse_usage;

 if (!mountpoint.beginswith("/"))
 {
   fprintf(stderr, "warning: assuming you gave a relative path with respect to current working directory => mountpoint=%s\n", mountpoint.c_str());
   XrdOucString pwd = getenv("PWD");
   if (!pwd.endswith("/"))
   {
     pwd += "/";
   }
   mountpoint.insert(pwd.c_str(), 0);
 }

 if (cmd == "mount")
 {
   struct stat buf;
   struct stat buf2;
   if (stat(mountpoint.c_str(), &buf))
   {
     XrdOucString createdir = "mkdir -p ";
     createdir += mountpoint;
     createdir += " >& /dev/null";
     fprintf(stderr, ".... trying to create ... %s\n", mountpoint.c_str());
     int rc = system(createdir.c_str());
     if (WEXITSTATUS(rc))
     {
       fprintf(stderr, "error: creation of mountpoint failed");
     }
   }


   if (stat(mountpoint.c_str(), &buf))
   {
     fprintf(stderr, "error: cannot create mountpoint %s !\n", mountpoint.c_str());
     exit(-1);
   }
   else
   {
     if (buf.st_dev == 19)
     {
       fprintf(stderr,"error: already/still mounted on %s !\n", mountpoint.c_str());
       exit(EBUSY);
     }
   }

   params += ",url=";
   params += serveruri.c_str();
   if ((params.find("//eos/") == STR_NPOS))
   {
     params += "//eos/";
   }

   fprintf(stderr, "===> Mountpoint   : %s\n", mountpoint.c_str());
   fprintf(stderr, "===> Fuse-Options : %s\n", params.c_str());
   if (logfile.length())
   {
     fprintf(stderr, "===> Log File     : %s\n", logfile.c_str());
     setenv("EOS_FUSE_LOGFILE", logfile.c_str(), 1);
   }

   XrdOucString env = "env";
   if (getenv("EOS_FUSE_READAHEADSIZE"))
   {
     env += " EOS_FUSE_READAHEADSIZE=";
     env += getenv("EOS_FUSE_READAHEADSIZE");
   }
   else
   {
     setenv("EOS_FUSE_READAHEADSIZE", "131072", 1);
     env += " EOS_FUSE_READAHEADSIZE=131072";
   }

   if (getenv("EOS_FUSE_READCACHESIZE"))
   {
     env += " EOS_FUSE_READCACHESIZE=";
     env += getenv("EOS_FUSE_READCACHESIZE");
   }
   else
   {
     setenv("EOS_FUSE_READCACHESIZE", "393216", 1);
     env += " EOS_FUSE_READCACHESIZE=393216";
   }

   if (getenv("EOS_FUSE_CACHE_SIZE"))
   {
     env += " EOS_FUSE_CACHE_SIZE=";
     env += getenv("EOS_FUSE_CACHE_SIZE");
   }
   else
   {
     setenv("EOS_FUSE_CACHE_SIZE", "100000000", 1);
     env += " EOS_FUSE_CACHE_SIZE=100000000";
   }

   if (getenv("EOS_FUSE_CACHE"))
   {
     env += " EOS_FUSE_CACHE=";
     env += getenv("EOS_FUSE_CACHE");
   }
   else
   {
     setenv("EOS_FUSE_CACHE", "1", 1);
     env += " EOS_FUSE_CACHE=1";
   }

   if (getenv("EOS_FUSE_DEBUG"))
   {
     env += " EOS_FUSE_DEBUG=";
     env += getenv("EOS_FUSE_DEBUG");
   }
   else
   {
     setenv("EOS_FUSE_DEBUG", "0", 1);
     env += " EOS_FUSE_DEBUG=0";
   }

   if (getenv("EOS_FUSE_LOWLEVEL_DEBUG"))
   {
     env += " EOS_FUSE_LOWOEVEL_DEBUG=";
     env += getenv("EOS_FUSE_LOWLEVEL_DEBUG");
   }
   else
   {
     setenv("EOS_FUSE_LOWLEVEL_DEBUG", "0", 1);
     env += " EOS_FUSE_LOWLEVEL_DEBUG=0";
   }

   if (getenv("EOS_FUSE_RMLVL_PROTECT"))
   {
     env += " EOS_FUSE_RMLVL_PROTECT=";
     env += getenv("EOS_FUSE_RMLVL_PROTECT");
   }
   else
   {
     setenv("EOS_FUSE_RMLVL_PROTECT", "1", 1);
     env += " EOS_FUSE_RMLVL_PROTECT=0";
   }

   if (getenv("EOS_FUSE_LAZYOPENRO"))
   {
     env += " EOS_FUSE_LAZYOPENRO=";
     env += getenv("EOS_FUSE_LAZYOPENRO");
   }
   else
   {
     setenv("EOS_FUSE_LAZYOPENRO", "0", 1);
     env += " EOS_FUSE_LAZYOPENRO=0";
   }

   if (getenv("EOS_FUSE_LAZYOPENRW"))
   {
     env += " EOS_FUSE_LAZYOPENRW=";
     env += getenv("EOS_FUSE_LAZYOPENRW");
   }
   else
   {
     setenv("EOS_FUSE_LAZYOPENRW", "1", 1);
     env += " EOS_FUSE_LAZYOPENRW=1";
   }

   bool mt=false;
   
   if (getenv("EOS_FUSE_NO_MT"))
   {
     env += " EOS_FUSE_NO_MT";
     env += getenv("EOS_FUSE_NO_MT");
     if (!strcmp("0", getenv("EOS_FUSE_NO_MT")))
       mt=true;
   }
   else
   {
     setenv("EOS_FUSE_NO_MT", "1", 1);
     env += " EOS_FUSE_NO_MT=1";
   }
   
   if (getenv("EOS_FUSE_LOGLEVEL"))
   {
     env += " EOS_FUSE_LOGLEVEL=";
     env += getenv("EOS_FUSE_LOGLEVEL");
   }
   else
   {
     setenv("EOS_FUSE_LOGLEVEL", "5", 1);
     env += " EOS_FUSE_LOGLEVEL=5";
   }
   
   fprintf(stderr, "===> xrootd ra             : %s\n", getenv("EOS_FUSE_READAHEADSIZE"));
   fprintf(stderr, "===> xrootd cache          : %s\n", getenv("EOS_FUSE_READCACHESIZE"));
   fprintf(stderr, "===> fuse debug            : %s\n", getenv("EOS_FUSE_DEBUG"));
   fprintf(stderr, "===> fuse low-level debug  : %s\n", getenv("EOS_FUSE_LOWLEVEL_DEBUG"));
   fprintf(stderr, "===> fuse log-level        : %s\n", getenv("EOS_FUSE_LOGLEVEL"));
   fprintf(stderr, "===> fuse write-cache      : %s\n", getenv("EOS_FUSE_CACHE"));
   fprintf(stderr, "===> fuse write-cache-size : %s\n", getenv("EOS_FUSE_CACHE_SIZE"));
   fprintf(stderr, "===> fuse rm level protect : %s\n", getenv("EOS_FUSE_RMLVL_PROTECT"));
   fprintf(stderr, "===> fuse lazy-open-ro     : %s\n", getenv("EOS_FUSE_LAZYOPENRO"));
   fprintf(stderr, "===> fuse lazy-open-rw     : %s\n", getenv("EOS_FUSE_LAZYOPENRW"));
   fprintf(stderr, "==== fuse multi-threading  : %s\n", mt?"true":"false");

   XrdOucString mount = env;
   mount += " eosd ";
   mount += mountpoint.c_str();
   mount += " -f";
   mount += " -o";
   mount += params;
   
   
   mount += " >& /dev/null ";
   int rc = system(mount.c_str());
   if (WEXITSTATUS(rc))
   {
     fprintf(stderr, "error: failed mount, check log for details\n");
     exit(-1);
   }
   
   bool mountok = false;

   // Keep checking for 5 seconds
   for (size_t i = 0; i < 50; i++)
   {
     if (stat(mountpoint.c_str(), &buf2) || (buf2.st_ino == buf.st_ino) )
     {
       usleep(100000);
     }
     else
     {
       mountok = true;
       break;
     }
   }

   if (!mountok)
   {
     fprintf(stderr, "error: failed mount at %s, check log for details\n",
             mountpoint.c_str());
     exit(-1);
   }
 }

 if (cmd == "umount")
 {
   struct stat buf2;

#ifndef __APPLE__
   struct stat buf1;
   XrdOucString pmount=mountpoint;
   if (pmount.endswith("/"))
     pmount.erase(pmount.length()-1);
   pmount.erase(pmount.rfind('/'));
   int r1 = stat(mountpoint.c_str(), &buf1);
   int r2 = stat(pmount.c_str(), &buf2);
   
   if ( ( r1 || r2) || (buf1.st_dev == buf2.st_dev) )
   {
     fprintf(stderr, "error: there is no eos mount at %s\n", mountpoint.c_str());
     exit(-1);
   }
#endif

   XrdOucString umount;
#ifdef __APPLE__
   umount = "umount -f ";
   umount += mountpoint.c_str();

#else
   umount = "fusermount -z -u ";
   umount += mountpoint.c_str();
#endif

   int rc = system(umount.c_str());
   if (WEXITSTATUS(rc))
   {
     fprintf(stderr, "error: umount failed\n");
   }
   if ((stat(mountpoint.c_str(), &buf2)))
   {
     fprintf(stderr, "error: mount directory disappeared from %s\n", mountpoint.c_str());
     exit(-1);
   }

#ifndef __APPLE__
   if (buf1.st_ino == buf2.st_ino)
   {
     fprintf(stderr, "error: umount didn't work\n");
     exit(-1);
   }
#endif
 }

 exit(0);

com_fuse_usage:
 fprintf(stdout, "usage: fuse mount  [-o <fuseparameterlist>] [-l <logfile>] <mount-point> : mount connected eos pool on <mount-point>\n");
 fprintf(stdout, "       fuse umount <mount-point>                                         : unmount eos pool from <mount-point>\n");
 exit(-1);
}
