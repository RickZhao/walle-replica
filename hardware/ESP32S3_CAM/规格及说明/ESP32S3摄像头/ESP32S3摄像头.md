
 #硬件模块 #HDMI #诱骗器 #带直通
 作业指导书见[[ZY705DESP32S3CAM01]]
## 产品概述
- **型号**：艾尔赛ESP32S3摄像头
- **特点**：
  - ESP32S3 8M RAM 16M FLASH
  - 拔插式摄像头，支持OV2640、OV5640等
  - 高亮闪光灯、SD卡插槽、可用IO 2.54排针引出
  - WIFI、蓝牙
## 技术参数
| 参数项  | 规格说明                          |
| ---- | ----------------------------- |
| 像素   | 200W像素(OV2640)、500W像素(OV5640) |
| 帧率   | 60HZ，向下兼容低帧率                  |
| 物理尺寸 | 27mm × 40mm                   |

## 接口介绍
![[Pasted image 20250711142348.png]]![[Pasted image 20250711142457.png]]![[Pasted image 20250711142710.png]]
 
## 使用指南
1.将摄像头插上模块接口上，扣好，通过TYPE-C线连接电脑
2.烧录程序，使用ARDUINO打开资料里的CameraWebServer，在41行处修改自己的WIFI名称密码。然后将设备通过TYPE-C接口连接到电脑，编译上传。(如有报错，安装最新的ARDUINO2.3.6，ESP32 3.2.1包)
![[Pasted image 20250711143257.png]]
3.点开ARDUINO的串口监视器，按摄像头模块上的RST复位键，在谷歌浏览器或微软EDGE浏览器上打开输出的网址，下滑找到Start Stream按钮，点击后显示摄像头画面。拖动LED Intensity滑块可点亮闪光灯
![[Pasted image 20250711144458.png]]
![[Pasted image 20250711144916.png]]




 
## 厂商信息
- **公司名称**：深圳市艾尔赛科技有限公司 (Shenzhen LC Technology Co., Ltd.)
- **联系地址**：
  - 深圳市龙华区大浪街道丽荣路1号国乐科技园3栋301
  - Room 301, Building No.3, Guole Technopark, Lirong Road, Dalang Street, Longhua District, Shenzhen 518110, China.