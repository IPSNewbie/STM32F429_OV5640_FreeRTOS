//
// Created by FAKE on 2026/5/28.
//
#include "bsp_softiic.h"
#include "gpio.h"
#include "delay_tim.h"
/**
  * 函    数：I2C写SCL引脚电平
  * 参    数：BitValue 协议层传入的当前需要写入SCL的电平，范围0~1
 */
  //该函数参数给1或0就可以释放或拉低SCL
void MyI2C_W_SCL(uint8_t BitValue)
{
	HAL_GPIO_WritePin(OV_SCL_GPIO_Port, OV_SCL_Pin,BitValue ? GPIO_PIN_SET : GPIO_PIN_RESET);		//根据BitValue，设置SCL引脚的电平
	delay_us(SCCB_DELAY_US);												//延时10us，防止时序频率超过从机的频率要求
}

/**
  * 函    数：I2C写SDA引脚电平
  * 参    数：BitValue 协议层传入的当前需要写入SDA的电平，范围0~1
  */
//该函数参数给1或0就可以释放或拉低SDA		（记住输入时要先释放总线！！！）
void MyI2C_W_SDA(uint8_t BitValue)
{
	HAL_GPIO_WritePin(OV_SDA_GPIO_Port, OV_SDA_Pin, BitValue ? GPIO_PIN_SET : GPIO_PIN_RESET);		//根据BitValue，设置SDA引脚的电平，BitValue要实现非0即1的特性
	delay_us(SCCB_DELAY_US);													//延时10us，防止时序频率超过从机的频率要求
}

/**
  * 函    数：I2C读SDA引脚电平
  * 返 回 值：协议层需要得到的当前SDA的电平，范围0~1
  */
uint8_t MyI2C_R_SDA(void)
{
	uint8_t BitValue;
	BitValue = HAL_GPIO_ReadPin(OV_SDA_GPIO_Port, OV_SDA_Pin);		//读取SDA电平
	delay_us(SCCB_DELAY_US);												//延时10us，防止时序频率超过从机的频率要求
	return BitValue;											//返回读到的SDA电平
}


//软件IIC的引脚初始化在MX_GPIO_Init()
// //调用 I2C_Init 函数后：
// 			//SCL（PA0） 和 SDA（PA1） 被初始化为 开漏输出模式。
// 			//SCL 和 SDA 均被置为高电平，表示 I2C 总线处于空闲状态。
// void MyI2C_Init(void)
// {
// 	//软件 I2C 仅使用 普通 GPIO 的读写函数（手动拉高/拉低电平），不需要STM32 的硬件 I2C 外设库函数。
//
// 	//选择GPIOA0、A1作为SCL、SDA，要初始化为开漏输出
// 	//①将 SCL 和 SDA 对应的 GPIO 引脚初始化为开漏输出模式
// 	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
//
// 	GPIO_InitTypeDef GPIO_Ininstructure;
// 	GPIO_Ininstructure.GPIO_Mode=GPIO_Mode_Out_OD;//GPIO处于开漏输出模式时，仍然可任意输入，只需要输入时，先输出1，再直接读取输入数据寄存器
// 	GPIO_Ininstructure.GPIO_Pin=GPIO_Pin_0 | GPIO_Pin_1;
// 	GPIO_Ininstructure.GPIO_Speed=GPIO_Speed_50MHz;
//
// 	GPIO_Init(GPIOA , &GPIO_Ininstructure);
//
// 	//②将 SCL 和 SDA 引脚电平置高，释放总线，表示I2C总线处于空闲状态
// 	GPIO_SetBits(GPIOA ,GPIO_Pin_0 | GPIO_Pin_1);
// }


//完成I2C时序的6个基本时序单元


//1、起始条件（兼容重复起始条件！！！）
void MyI2C_Start(void)
{
	//首先把SCL和SDA都确保释放，然后先拉低SDA,再拉低SCL,这样就能产生起始条件了。

	MyI2C_W_SDA(1);							//兼容重复起始条件！先释放SDA，确保SDA为高电平（见注释）
	MyI2C_W_SCL(1);							//释放SCL，确保SCL为高电平
	MyI2C_W_SDA(0);							//在SCL高电平期间，拉低SDA，产生起始信号
	MyI2C_W_SCL(0);							//起始后把SCL也拉低，即为了占用总线，也为了方便总线时序的拼接
}

//2、终止条件
void MyI2C_Stop(void)
{
	MyI2C_W_SDA(0);							//拉低SDA，确保SDA为低电平
	MyI2C_W_SCL(1);							//释放SCL，使SCL呈现高电平
	MyI2C_W_SDA(1);							//在SCL高电平期间，释放SDA，产生终止信号
}

//3、发送1个字节
void MyI2C_SendByte(uint8_t Byte)
{
	// 除了终止条件（Stop）会使 SCL 以高电平结束外，所有其他 I2C 时序单元（如起始、发送字节、应答等）都保证 SCL 以低电平结束。
	//所以后续可以直接发送数据

	uint8_t i;
	for (i = 0; i < 8; i ++)				//循环8次，主机依次发送数据的每一位
	{
		//两个!可以对数据进行两次逻辑取反，作用是把非0值统一转换为1，即：!!(0) = 0，!!(非0) = 1
		MyI2C_W_SDA(!!(Byte & (0x80 >> i)));//使用掩码的方式取出Byte的指定一位数据并写入到SDA线
		MyI2C_W_SCL(1);						//释放SCL，从机在SCL高电平期间读取SDA
		MyI2C_W_SCL(0);						//拉低SCL，主机开始发送下一位数据
	}
}

//4、接受1个字节
uint8_t MyI2C_ReceiveByte()
{
	uint8_t Byte=0x00;								//定义接收的数据，并赋初值0x00，此处必须赋初值0x00，后面会用到
	MyI2C_W_SDA(1);									//接收前，主机先确保释放SDA，避免干扰从机的数据发送

	for(uint8_t i = 0; i < 8; i++)					//循环8次，主机依次接收数据的每一位
	{
		MyI2C_W_SCL(1);								//释放SCL，主机机在SCL高电平期间读取SDA
//		Delay_us(10);								//从机在SCL上升沿瞬间将数据放入SDA，需要时间，等待数据稳定
		if (MyI2C_R_SDA()){Byte |= (0x80 >> i);}	//读取SDA数据，并存储到Byte变量
														//当SDA为1时，置变量指定位为1，当SDA为0时，不做处理，指定位为默认的初值0
		MyI2C_W_SCL(0);								//拉低SCL，从机在SCL低电平期间写入SDA
	}

	return Byte;
}

//5、发送应答(主机接收1字节后，要向从机发送1位应答数据)
void MyI2C_SendAck(uint8_t AckBit)
{
	// 除了终止条件（Stop）会使 SCL 以高电平结束外，所有其他 I2C 时序单元（如起始、发送字节、应答等）都保证 SCL 以低电平结束。
	//所以后续可以直接发送数据

	//两个!可以对数据进行两次逻辑取反，作用是把非0值统一转换为1，即：!!(0) = 0，!!(非0) = 1
	MyI2C_W_SDA(!!AckBit);				//主机把应答位数据放到SDA线
	MyI2C_W_SCL(1);						//释放SCL，从机在SCL高电平期间，读取应答位
	MyI2C_W_SCL(0);						//拉低SCL，开始下一个时序模块
}

//6、接收应答(主机发送1字节后，要接收从机发送的1位应答数据)
uint8_t MyI2C_ReceiveAck()
{
	uint8_t AckBit;							//定义应答位变量
	MyI2C_W_SDA(1);							//接收前，主机先确保释放SDA，避免干扰从机的数据发送

	MyI2C_W_SCL(1);							//释放SCL，主机机在SCL高电平期间读取SDA
//	Delay_us(10);							// 等待数据稳定
	AckBit = MyI2C_R_SDA();					//将应答位存储到变量里
	MyI2C_W_SCL(0);							//拉低SCL，开始下一个时序模块

	return AckBit;							//返回定义应答位变量
}
