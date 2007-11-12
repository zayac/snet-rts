#include <Pbox.h>
#include <cwrapper.h>
#include <sacinterface.h>

void *Pbox( void *hnd, void *ptr_1)
{
  SACarg *sac_result;
  
  desboxes__Pbox1( &sac_result, ptr_1);
  
  SAC2SNet_outRaw( hnd, 1, sac_result);
  

  return( hnd);
}
