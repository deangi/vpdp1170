// Arduino's sketch build compiles .cpp files in the sketch root but does NOT
// recursively descend into arbitrary subdirectories like SD_FTP_Server/src/.
// This shim pulls the library's implementation in as a single TU so the
// sketch links cleanly without the user having to install the library into
// their global Arduino libraries folder.
//
// For use OUTSIDE this sketch, just copy SD_FTP_Server/ to ~/Documents/
// Arduino/libraries/ and #include <SD_FTP_Server.h> normally.
#include "SD_FTP_Server/src/SD_FTP_Server.cpp"
