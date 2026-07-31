#include<reg52.h>           
#include <intrins.h>  
typedef  unsigned char  uchar;        
typedef  unsigned int   uint;      
  
sbit scl=P2^6;                      
sbit sda=P2^7;                  
#define LU9685_adrr1 0x00				// LU9685_adrr1=A4*32+A3*16+A2*8+A1*4+A0*2,LU9685模块上面焊点短接的话Ax=1，否则为0

void delayms(uint z)
{
  uint x,y;
  for(x=z;x>0;x--)
      for(y=110;y>0;y--);
}
/********************************************************************
发送一个字节串口数据
********************************************************************/ 
void Uart_Send_Byte(unsigned char c)//UART Send a byte
{
	SBUF = c;
	while(!TI);		//发送完为1 
	TI = 0;
}
/********************************************************************
* 名称 : UART_Init()
* 功能 : 单片机串口初始化
* 单片机：晶振11.0592MHz 波特率为9600bps
* 舵机模块：波特率为9600bps，
********************************************************************/        
void UART_Init()
{
	SCON= 0x50;               //串口方式1	//REN=1; 允许接收
	PCON=0x00;                //SMOD=0
	TMOD= 0x22;               //定时器1定时方式2
	TH1= 0xFD;                //11.0592MHz  模块默认波特率为9600bps
	TL1= 0xFD;										 
	TR1= 1;                   //启动定时器 
	EA=1;   
}
/*---------------------------------------------------------------
	adder：模块的硬件地址
	num：通道 0-15
	off为0-180,off大于等于200时不输出信号，此时释放舵机
----------------------------------------------------------------*/
void setPWM(uchar adder,uchar num,uchar off) 
{
	Uart_Send_Byte(0XFA);
	Uart_Send_Byte(adder);
	Uart_Send_Byte(num);
	Uart_Send_Byte(off);
	Uart_Send_Byte(0XFE);
}
void LU9685_reset(uchar adder) 
{
	Uart_Send_Byte(0XFA);
	Uart_Send_Byte(adder);
	Uart_Send_Byte(0XFB);
	Uart_Send_Byte(0XFB);
	Uart_Send_Byte(0XFE);
}
void main()
{   
	uchar m,n;
	//串口初始化
    UART_Init();
	//LU9685软复位
	LU9685_reset(LU9685_adrr1);         
    while(1)
	{
		for(n=0;n<16;n++)
		{
			for(m=0;m<=180;m++)
			{
				setPWM(LU9685_adrr1,n,m);						
			}					
		}
		delayms(500);	
		for(n=0;n<16;n++)
		{
			for(m=0;m<=180;m++)
			{
				setPWM(LU9685_adrr1,15-n,180-m);						
			}					
		}		
		delayms(500);											
	}
}