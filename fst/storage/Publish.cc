// ----------------------------------------------------------------------
// File: Publish.cc
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

/*----------------------------------------------------------------------------*/
#include "fst/storage/Storage.hh"
#include "fst/XrdFstOfs.hh"
#include "common/LinuxStat.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

/*----------------------------------------------------------------------------*/
void
Storage::Publish ()
{
 eos_static_info("Publisher activated ...");
 struct timeval tv1, tv2;
 struct timezone tz;
 unsigned long long netspeed = 1000000000;
 // ---------------------------------------------------------------------
 // get our network speed
 // ---------------------------------------------------------------------
 char* tmpname = tmpnam(NULL);
 XrdOucString getnetspeed = "ip route list | sed -ne '/^default/s/.*dev //p' | xargs ethtool | grep Speed | cut -d ':' -f2 | cut -d 'M' -f1 >> ";
 getnetspeed += tmpname;
 system(getnetspeed.c_str());

 XrdOucString lNodeGeoTag = (getenv("EOS_GEOTAG") ? getenv("EOS_GEOTAG") : "");

 FILE* fnetspeed = fopen(tmpname, "r");
 if (fnetspeed)
 {
   if ((fscanf(fnetspeed, "%llu", &netspeed)) == 1)
   {
     // we get MB as a number => convert into bytes
     netspeed *= 1000000;
     eos_static_info("ethtool:networkspeed=%.02f GB/s", 1.0 * netspeed / 1000000000.0);
   }
   fclose(fnetspeed);
 }

 eos_static_info("publishing:networkspeed=%.02f GB/s", 1.0 * netspeed / 1000000000.0);

 // ---------------------------------------------------------------------
 // give some time before publishing
 // ---------------------------------------------------------------------
 XrdSysTimer sleeper;
 sleeper.Snooze(3);

 while (!eos::fst::Config::gConfig.FstNodeConfigQueue.length())
 {
   XrdSysTimer sleeper;
   sleeper.Snooze(5);
   eos_static_info("Snoozing ...");
 }

 eos::common::FileSystem::fsid_t fsid = 0;
 std::string publish_uptime = "";
 std::string publish_sockets = "";

 while (1)
 {
   {
     // ---------------------------------------------------------------------
     // retrieve uptime information
     // ---------------------------------------------------------------------
     XrdOucString uptime = "uptime | tr -d \"\n\" > ";
     uptime += tmpname;
     int rc = system(uptime.c_str());
     if (WEXITSTATUS(rc))
     {
       eos_static_err("retrieve uptime call failed");
     }
     eos::common::StringConversion::LoadFileIntoString(tmpname, publish_uptime);
     XrdOucString sockets = "cat /proc/net/tcp | wc -l | tr -d \"\n\" >";
     sockets += tmpname;
     rc = system(sockets.c_str());
     if (WEXITSTATUS(rc))
     {
       eos_static_err("retrieve #socket call failed");
     }
     eos::common::StringConversion::LoadFileIntoString(tmpname, publish_sockets);
   }

   time_t now = time(NULL);
   gettimeofday(&tv1, &tz);

   // TODO: derive this from a global variable
   // smear the publishing cycle around x +- x/2 seconds
   int PublishInterval = 10;
   {
     XrdSysMutexHelper lock(eos::fst::Config::gConfig.Mutex);
     PublishInterval = eos::fst::Config::gConfig.PublishInterval;
   }
   if ((PublishInterval < 2) || (PublishInterval > 3600))
   {
     // default to 10 +- 5 seconds
     PublishInterval = 10;
   }

   unsigned int lReportIntervalMilliSeconds = (PublishInterval * 500)+ (unsigned int) ((PublishInterval * 1000.0) * rand() / RAND_MAX);

   // retrieve the process memory and thread state

   eos::common::LinuxStat::linux_stat_t osstat;

   if (!eos::common::LinuxStat::GetStat(osstat))
   {
     eos_err("failed to get the memory usage information");
   }

   {
     // run through our defined filesystems and publish with a MuxTransaction all changes
     eos::common::RWMutexReadLock lock(fsMutex);
     static time_t last_consistency_stats = 0;
     static time_t next_consistency_stats = 0;

     if (!gOFS.ObjectManager.OpenMuxTransaction())
     {
       eos_static_err("cannot open mux transaction");
     }
     else
     {
       // copy out statfs info
       for (size_t i = 0; i < fileSystemsVector.size(); i++)
       {
         if (!fileSystemsVector[i])
         {
           eos_static_err("found 0 vector in filesystems vector %u", i);
           continue;
         }

         if (!(fsid = fileSystemsVector[i]->GetId()))
         {
           // during the boot phase we can find a filesystem without ID
           continue;
         }

         // Retrieve Statistics from the SQLITE DB
         std::map<std::string, size_t>::const_iterator isit;


         bool success = true;
         if (fileSystemsVector[i]->GetStatus() == eos::common::FileSystem::kBooted)
         {
           if (next_consistency_stats < now)
           {
             eos_static_debug("msg=\"publish consistency stats\"");
             last_consistency_stats = now;
             XrdSysMutexHelper ISLock(fileSystemsVector[i]->InconsistencyStatsMutex);
             gFmdSqliteHandler.GetInconsistencyStatistics(fsid, *fileSystemsVector[i]->GetInconsistencyStats(), *fileSystemsVector[i]->GetInconsistencySets());
             for (isit = fileSystemsVector[i]->GetInconsistencyStats()->begin(); isit != fileSystemsVector[i]->GetInconsistencyStats()->end(); isit++)
             {
               //eos_static_debug("%-24s => %lu", isit->first.c_str(), isit->second);
               std::string sname = "stat.fsck.";
               sname += isit->first;
               success &= fileSystemsVector[i]->SetLongLong(sname.c_str(), isit->second);
             }
           }
         }

         eos::common::Statfs* statfs = 0;
         if ((statfs = fileSystemsVector[i]->GetStatfs()))
         {
           // call the update function which stores into the filesystem shared hash
           if (!fileSystemsVector[i]->SetStatfs(statfs->GetStatfs()))
           {
             eos_static_err("cannot SetStatfs on filesystem %s", fileSystemsVector[i]->GetPath().c_str());
           }
         }

         // copy out net info 
         // TODO: take care of eth0 only ..
         // somethimg has to tell us if we are 1GBit, or 10GBit ... we assume 1GBit now as the default
         success &= fileSystemsVector[i]->SetDouble("stat.net.ethratemib", netspeed / (8 * 1024 * 1024));
         success &= fileSystemsVector[i]->SetDouble("stat.net.inratemib", fstLoad.GetNetRate("eth0", "rxbytes") / 1024.0 / 1024.0);
         success &= fileSystemsVector[i]->SetDouble("stat.net.outratemib", fstLoad.GetNetRate("eth0", "txbytes") / 1024.0 / 1024.0);
         //          eos_static_debug("Path is %s %f\n", fileSystemsVector[i]->GetPath().c_str(), fstLoad.GetDiskRate(fileSystemsVector[i]->GetPath().c_str(),"writeSectors")*512.0/1000000.0);
         success &= fileSystemsVector[i]->SetDouble("stat.disk.readratemb", fstLoad.GetDiskRate(fileSystemsVector[i]->GetPath().c_str(), "readSectors")*512.0 / 1000000.0);
         success &= fileSystemsVector[i]->SetDouble("stat.disk.writeratemb", fstLoad.GetDiskRate(fileSystemsVector[i]->GetPath().c_str(), "writeSectors")*512.0 / 1000000.0);
         success &= fileSystemsVector[i]->SetDouble("stat.disk.load", fstLoad.GetDiskRate(fileSystemsVector[i]->GetPath().c_str(), "millisIO") / 1000.0);
         gOFS.OpenFidMutex.Lock();
         success &= fileSystemsVector[i]->SetLongLong("stat.ropen", (long long) gOFS.ROpenFid[fsid].size());
         success &= fileSystemsVector[i]->SetLongLong("stat.wopen", (long long) gOFS.WOpenFid[fsid].size());
         success &= fileSystemsVector[i]->SetLongLong("stat.statfs.freebytes", fileSystemsVector[i]->GetLongLong("stat.statfs.bfree") * fileSystemsVector[i]->GetLongLong("stat.statfs.bsize"));
         success &= fileSystemsVector[i]->SetLongLong("stat.statfs.usedbytes", (fileSystemsVector[i]->GetLongLong("stat.statfs.blocks") - fileSystemsVector[i]->GetLongLong("stat.statfs.bfree")) * fileSystemsVector[i]->GetLongLong("stat.statfs.bsize"));
         success &= fileSystemsVector[i]->SetDouble("stat.statfs.filled", 100.0 * ((fileSystemsVector[i]->GetLongLong("stat.statfs.blocks") - fileSystemsVector[i]->GetLongLong("stat.statfs.bfree"))) / (1 + fileSystemsVector[i]->GetLongLong("stat.statfs.blocks")));
         success &= fileSystemsVector[i]->SetLongLong("stat.statfs.capacity", fileSystemsVector[i]->GetLongLong("stat.statfs.blocks") * fileSystemsVector[i]->GetLongLong("stat.statfs.bsize"));
         success &= fileSystemsVector[i]->SetLongLong("stat.statfs.fused", (fileSystemsVector[i]->GetLongLong("stat.statfs.files") - fileSystemsVector[i]->GetLongLong("stat.statfs.ffree")) * fileSystemsVector[i]->GetLongLong("stat.statfs.bsize"));
         {
           eos::common::RWMutexReadLock lock(gFmdSqliteHandler.Mutex);
           success &= fileSystemsVector[i]->SetLongLong("stat.usedfiles", (long long) (gFmdSqliteHandler.FmdSqliteMap.count(fsid) ? gFmdSqliteHandler.FmdSqliteMap[fsid].size() : 0));
         }

         success &= fileSystemsVector[i]->SetString("stat.boot", fileSystemsVector[i]->GetString("stat.boot").c_str());
         success &= fileSystemsVector[i]->SetString("stat.geotag", lNodeGeoTag.c_str());
         success &= fileSystemsVector[i]->SetLongLong("stat.drainer.running", fileSystemsVector[i]->GetDrainQueue()->GetRunningAndQueued());
         success &= fileSystemsVector[i]->SetLongLong("stat.balancer.running", fileSystemsVector[i]->GetBalanceQueue()->GetRunningAndQueued());

         gOFS.OpenFidMutex.UnLock();

         {
           XrdSysMutexHelper(fileSystemFullMapMutex);
           long long fbytes = fileSystemsVector[i]->GetLongLong("stat.statfs.freebytes");
           // stop the writers if it get's critical under 5 GB space

           if ((fbytes < 5 * 1024ll * 1024ll * 1024ll))
           {
             fileSystemFullMap[fsid] = true;
           }
           else
           {
             fileSystemFullMap[fsid] = false;
           }

           if ((fbytes < 1024ll * 1024ll * 1024ll) || (fbytes <= fileSystemsVector[i]->GetLongLong("headroom")))
           {
             fileSystemFullWarnMap[fsid] = true;
           }
           else
           {
             fileSystemFullWarnMap[fsid] = false;
           }
         }

         if (!success)
         {
           eos_static_err("cannot set net parameters on filesystem %s", fileSystemsVector[i]->GetPath().c_str());
         }
       }

       {
         // set node status values
         gOFS.ObjectManager.HashMutex.LockRead();
         // we received a new symkey 
         XrdMqSharedHash* hash = gOFS.ObjectManager.GetObject(Config::gConfig.FstNodeConfigQueue.c_str(), "hash");
         if (hash)
         {
           hash->Set("stat.sys.kernel", eos::fst::Config::gConfig.KernelVersion.c_str());
           hash->SetLongLong("stat.sys.vsize", osstat.vsize);
           hash->SetLongLong("stat.sys.rss", osstat.rss);
           hash->SetLongLong("stat.sys.threads", osstat.threads);
           hash->Set("stat.sys.eos.version", VERSION);
           hash->Set("stat.sys.keytab", eos::fst::Config::gConfig.KeyTabAdler.c_str());
           hash->Set("stat.sys.uptime", publish_uptime.c_str());
           hash->Set("stat.sys.sockets", publish_sockets.c_str());
           hash->Set("stat.sys.eos.start", eos::fst::Config::gConfig.StartDate.c_str());
         }
         gOFS.ObjectManager.HashMutex.UnLockRead();
       }
       gOFS.ObjectManager.CloseMuxTransaction();
       next_consistency_stats = last_consistency_stats + 60; // report the consistency only once per minute
     }
   }
   gettimeofday(&tv2, &tz);
   int lCycleDuration = (int) ((tv2.tv_sec * 1000.0)-(tv1.tv_sec * 1000.0) + (tv2.tv_usec / 1000.0) - (tv1.tv_usec / 1000.0));
   int lSleepTime = lReportIntervalMilliSeconds - lCycleDuration;
   eos_static_debug("msg=\"publish interval\" %d %d", lReportIntervalMilliSeconds, lCycleDuration);
   if (lSleepTime < 0)
   {
     eos_static_warning("Publisher cycle exceeded %d millisecons - took %d milliseconds", lReportIntervalMilliSeconds, lCycleDuration);
   }
   else
   {
     XrdSysTimer sleeper;
     sleeper.Snooze(lSleepTime / 1000);
   }
 }
}

EOSFSTNAMESPACE_END

