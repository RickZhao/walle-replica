#include "stm32f10x.h"
#include "delay.h"
#include "sys.h"
#include "pca9685.h"
#include "myiic.h"
u16 addr1; 
//16通道循环做0°-180°-0°往返运动
//如果要释放电机（即不输出PWM信号）,那么请将该通道角度设为200即可。
 int main(void)
 {	
		u16 n,m;
	  //模块默认地址为0X00
	  addr1=0X00;
    delay_init();
		//初始化IO口
	 	IIC_Init();
	 //复位模块
	  pca_reset(addr1);
	  //20路舵机全部角度归零
		for(n=0;n<20;n++)   
			data_20[n]=0;
		pca_write20(addr1);
		delay_ms(10);
		while(1)
		{			
			for(n=0;n<16;n++)
			{
					for(m=0;m<=180;m++)
					{
						//addr1:模块地址,n：通道,m：舵机角度数据
							pca_setpwm(addr1,n,m);	
							delay_ms(2);						
					}					
			}
			delay_ms(500);	
			for(n=0;n<16;n++)
			{
					for(m=0;m<=180;m++)
					{
							pca_setpwm(addr1,15-n,180-m);	
							delay_ms(2);						
					}					
			}		
			delay_ms(500);				
	 }
}
 


