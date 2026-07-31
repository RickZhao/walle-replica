#include "pca9685.h"
#include "myiic.h"
#include "delay.h"
#include "math.h"
u8 data_20[20];
void pca_write20(u8 pca_adr)
{ 
	 IIC_Start();
	
	 IIC_Send_Byte(pca_adr);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(0XFD);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[0]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[1]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[2]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[3]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[4]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[5]);
	 IIC_Wait_Ack();	
	 
	 IIC_Send_Byte(data_20[6]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[7]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[8]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[9]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[10]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[11]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[12]);
	 IIC_Wait_Ack();

	 IIC_Send_Byte(data_20[13]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[14]);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(data_20[15]);
	 IIC_Wait_Ack();
	 
	 IIC_Send_Byte(data_20[16]);
	 IIC_Wait_Ack();
	 
	 IIC_Send_Byte(data_20[17]);
	 IIC_Wait_Ack();
	 
	 IIC_Send_Byte(data_20[18]);
	 IIC_Wait_Ack();
	 
	 IIC_Send_Byte(data_20[19]);
	 IIC_Wait_Ack();
   IIC_Stop();
}
void pca_reset(u8 pca_adr)
{
	IIC_Start();

	IIC_Send_Byte(pca_adr);
	IIC_Wait_Ack();

	IIC_Send_Byte(0XFB);
	IIC_Wait_Ack();

	IIC_Send_Byte(0XFB);
	IIC_Wait_Ack();

	IIC_Stop();
}
void pca_setpwm(u8 pca_adr,u8 num, u8 off) 
{
	 IIC_Start();
	
	 IIC_Send_Byte(pca_adr);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(num);
	 IIC_Wait_Ack();
	
	 IIC_Send_Byte(off);
	 IIC_Wait_Ack();
	
   IIC_Stop();
}
