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
void delayus()  //大于4.7us
{
	_nop_();         
	_nop_();
	_nop_();
	_nop_();
	_nop_();

}
/*---------------------------------------------------------------
                 IIC初始化       
----------------------------------------------------------------*/
void init()
{
    sda=1;                //sda scl使用之前被拉高
    delayus();
    scl=1;
    delayus();
}
/*---------------------------------------------------------------
                 IIC总线开始传输
----------------------------------------------------------------*/
void start()
{
    sda=1;
    delayus();
    scl=1;       //scl高sda拉低 
    delayus();
    sda=0;
    delayus();
    scl=0;
    delayus();
}
/*---------------------------------------------------------------
                 IIC结束
----------------------------------------------------------------*/
void stop()
{
    sda=0;
    delayus();
    scl=1;                         //scl拉高 sda产生上升沿
    delayus();
    sda=1;                   
    delayus();
}
/*---------------------------------------------------------------
                 IIC应答
----------------------------------------------------------------*/
void ACK()
{
    uchar i;
    scl=1;
    delayus();
    while((sda=1)&&(i<255))         
     i++;                                        
    scl=0;                                  
    delayus();
}
/*---------------------------------------------------------------
              写一个字节
----------------------------------------------------------------*/
void write_byte(uchar byte)
{
    uchar i,temp;
    temp=byte;
    for(i=0;i<8;i++)
    {
        temp=temp<<1;  
        scl=0;                  
        delayus();
        sda=CY;                 
        delayus();
        scl=1;           
        delayus();
    }
    scl=0;                  
    delayus();
    sda=1;                 
    delayus();
}
/*---------------------------------------------------------------
	adder：模块的硬件地址
	num：通道 0-15
	off为0-180,off大于等于200时不输出信号，此时释放舵机
----------------------------------------------------------------*/
void setPWM(uchar adder,uchar num,uchar off) 
{
	start();
	write_byte(adder);       
	ACK();                          
	write_byte(num);  
	ACK();
	write_byte(off); 
	ACK();
	stop();
}
void LU9685_reset(uchar adder) 
{
	start();
	write_byte(adder);       
	ACK();                          
	write_byte(0xFB);  
	ACK();
	write_byte(0xFB); 
	ACK();
	stop();
}
void main()
{   
	uchar m,n;
	//IIC-IO初始化
    init();
	//软复位
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