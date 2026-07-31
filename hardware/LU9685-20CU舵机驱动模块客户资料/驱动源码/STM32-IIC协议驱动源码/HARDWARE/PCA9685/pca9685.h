#ifndef __PCA_H
#define __PCA_H	
#include "stm32f10x.h"

#define pca_adr1 0x80

extern u8 data_20[20];

void pca_write20(u8 pca_adr);
void pca_reset(u8 pca_adr);
void pca_setpwm(u8 pca_adr,u8 num, u8 off);


#endif



