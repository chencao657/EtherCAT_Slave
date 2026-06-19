#ifndef __ECAT_IO_APP_H__
#define __ECAT_IO_APP_H__

#include "ecat_def.h"

UINT8 EcatIo_ReadInputs(void);
void EcatIo_ApplyOutputs(UINT8 outputByte);
void EcatIo_Application(void);

#endif /* __ECAT_IO_APP_H__ */
