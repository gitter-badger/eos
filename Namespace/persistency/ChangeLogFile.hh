//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   ChangeLog like store
//------------------------------------------------------------------------------

#ifndef EOS_CHANGE_LOG_FILE_HH
#define EOS_CHANGE_LOG_FILE_HH

#include <string>
#include <stdint.h>
#include <ctime>
#include "Namespace/persistency/Buffer.hh"
#include "Namespace/utils/Descriptor.hh"
#include "Namespace/MDException.hh"

namespace eos
{
  //----------------------------------------------------------------------------
  //! Interface for a class scanning the logfile
  //----------------------------------------------------------------------------
  class ILogRecordScanner
  {
    public:
      //------------------------------------------------------------------------
      //! Process record
      //------------------------------------------------------------------------
      virtual void processRecord( uint64_t offset, char type,
                                  const Buffer &buffer ) = 0;
  };

  //----------------------------------------------------------------------------
  //! Statistics of the repair process
  //----------------------------------------------------------------------------
  struct LogRepairStats
  {
    LogRepairStats(): fixedWrongMagic(0), fixedWrongSize(0), fixedWrongChecksum(0),
      notFixed(0), scanned(0), healthy(0), bytesDiscarded(0), bytesAccepted(0),
      bytesTotal(0), timeElapsed(0) {}

    uint64_t fixedWrongMagic;
    uint64_t fixedWrongSize;
    uint64_t fixedWrongChecksum;
    uint64_t notFixed;
    uint64_t scanned;
    uint64_t healthy;
    uint64_t bytesDiscarded;
    uint64_t bytesAccepted;
    uint64_t bytesTotal;
    time_t   timeElapsed;
  };

  //----------------------------------------------------------------------------
  //! Feedback from the changelog reparation process
  //----------------------------------------------------------------------------
  class ILogRepairFeedback
  {
    public:
      //------------------------------------------------------------------------
      //! Called to report progress to the outside world
      //------------------------------------------------------------------------
      virtual void reportProgress( LogRepairStats &stats ) = 0;
  };

  //----------------------------------------------------------------------------
  //! Changelog like store
  //----------------------------------------------------------------------------
  class ChangeLogFile
  {
    public:
      //------------------------------------------------------------------------
      //! Constructor
      //------------------------------------------------------------------------
      ChangeLogFile(): pIsOpen( false ), pVersion( 0 ) {};

      //------------------------------------------------------------------------
      //! Destructor
      //------------------------------------------------------------------------
      virtual ~ChangeLogFile() {};

      //------------------------------------------------------------------------
      //! Open the log file
      //------------------------------------------------------------------------
      void open( const std::string &name ) throw( MDException );

      //------------------------------------------------------------------------
      //! Check if the changelog file is opened already
      //------------------------------------------------------------------------
      bool isOpen() const
      {
        return pIsOpen;
      }

      //------------------------------------------------------------------------
      //! Close the log
      //------------------------------------------------------------------------
      void close();

      //------------------------------------------------------------------------
      //! Sync the buffers to disk
      //------------------------------------------------------------------------
      void sync() throw( MDException );

      //------------------------------------------------------------------------
      //! Store the record in the log
      //!
      //! @param type   user defined type of record
      //! @param record a record buffer, it is not const because zeros may be
      //!               appended to the end to make it aligned to 4 bytes
      //!
      //! @return the offset in the log
      //------------------------------------------------------------------------
      uint64_t storeRecord( char type, Buffer &record ) throw( MDException );

      //------------------------------------------------------------------------
      //! Read the record at given offset
      //------------------------------------------------------------------------
      uint8_t readRecord( uint64_t offset, Buffer &record ) throw( MDException);

      //------------------------------------------------------------------------
      //! Scan all the records in the changelog file
      //------------------------------------------------------------------------
      void scanAllRecords( ILogRecordScanner *scanner ) throw( MDException );

      //------------------------------------------------------------------------
      //! Follow a file
      //!
      //! @param scanner a listener to be notified about a new record
      //! @param poll    look for new data every poll microseconds
      //------------------------------------------------------------------------
      void follow( ILogRecordScanner* scanner, unsigned poll = 100000 )
        throw( MDException );

      //------------------------------------------------------------------------
      //! Repair a changelog file
      //!
      //! @param filename    name of the file to be repaired (read only)
      //! @param newFilename placeholder for the fixed records
      //! @param feedback    instance of a feedback class to determine reactions
      //!                    to problems
      //! @param stats       placeholder for the statistics
      //------------------------------------------------------------------------
      static void repair( const std::string  &filename,
                          const std::string  &newFilename,
                          LogRepairStats     &stats,
                          ILogRepairFeedback *feedback ) throw( MDException );
    private:

      //------------------------------------------------------------------------
      // Data members
      //------------------------------------------------------------------------
      int      pFd;
      bool     pIsOpen;
      uint8_t  pVersion;
      uint64_t pSeqNumber;
  };
}

#endif // EOS_CHANGE_LOG_FILE_HH
