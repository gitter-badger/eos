/*----------------------------------------------------------------------------*/
#include "ConsoleMain.hh"
/*----------------------------------------------------------------------------*/

/* Clear the terminal screen */
int
com_clear (char *arg) {
  system("clear");
  return (0);
}
