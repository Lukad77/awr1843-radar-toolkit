
#include "WzSerialportPlus.h"

#include <stdio.h>
#include <string.h>
#include <chrono>

#ifdef _WIN32
// Windows 无效句柄
static const SerialHandle kInvalidSerial = INVALID_HANDLE_VALUE;
#else
// POSIX(Mac/Linux) 串口实现所需
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static const SerialHandle kInvalidSerial = -1;

// 把整型波特率映射为 termios 的 speed_t 常量。
// 返回 true 表示支持；对不认识的波特率返回 false。
static bool baudrateToSpeed(int baudrate, speed_t& out) {
    switch (baudrate) {
        case 2400:   out = B2400;   return true;
        case 4800:   out = B4800;   return true;
        case 9600:   out = B9600;   return true;
        case 19200:  out = B19200;  return true;
        case 38400:  out = B38400;  return true;
        case 57600:  out = B57600;  return true;
        case 115200: out = B115200; return true;
        case 230400: out = B230400; return true;
#ifdef B460800
        case 460800: out = B460800; return true;
#endif
#ifdef B921600
        case 921600: out = B921600; return true;  // AWR1843 数据口默认
#endif
        default: return false;
    }
}
#endif


WzSerialportPlus::WzSerialportPlus()
    : serialportHandle(kInvalidSerial),
        name(""),
        baudrate(9600),
        stopbit(1),
        databit(8),
        paritybit('n'),
        receivable(false),
        receiveMaxlength(92160),
        receiveTimeout(5000),
        receiveCallback(nullptr)
{

}

WzSerialportPlus::WzSerialportPlus(const std::string& name,
                    const int& baudrate,
                    const int& stopbit,
                    const int& databit,
                    const int& paritybit)
		:serialportHandle(kInvalidSerial),
            name(name),
            baudrate(baudrate),
            stopbit(stopbit),
            databit(databit),
            paritybit(paritybit),
            receivable(false),
            receiveMaxlength(92160),
            receiveTimeout(5000),
            receiveCallback(nullptr)
{

}

WzSerialportPlus::~WzSerialportPlus()
{
    close();

    while(receivable)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // printf("[WzSerialportPlus::~WzSerialportPlus()]: destructed...\n");
}

bool WzSerialportPlus::open()
{
#ifdef _WIN32
	serialportHandle = CreateFileA(name.c_str(),
		GENERIC_READ | GENERIC_WRITE,
		0, 
		NULL,
		OPEN_EXISTING, 
		0, 
		NULL);

	if (serialportHandle == (HANDLE)-1)
	{
        printf("[WzSerialportPlus::open()]: open failed with serialportHandle is %d , maybe permission denied or this serialport is opened!\n", (int)serialportHandle);
		return false;
	}
	
	if (!SetupComm(serialportHandle, receiveMaxlength, receiveMaxlength))
	{
		printf("[WzSerialportPlus::open()]: open failed with cannot setup io buffer size!\n");
		CloseHandle(serialportHandle);
		return false;
	}

	/* 
	 * parameters config:
	 *		baudrate,databit,paritybit,stopbit 
	 */
	DCB parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.DCBlength = sizeof(parameters);
	parameters.BaudRate = baudrate; 
	parameters.ByteSize = databit; 
	switch (paritybit)
	{
	case 'n':
    case 'N':
		parameters.Parity = NOPARITY;
		// parameters.fParity = FALSE;
		break;
	case 'o':
	case 'O':
		parameters.Parity = ODDPARITY;
		break;
	case 'e':
    case 'E': 
		parameters.Parity = EVENPARITY;
		break;
	case 3:
		parameters.Parity = MARKPARITY;
		break;
	case 4:
		parameters.Parity = SPACEPARITY;
		break;
	}
	switch (stopbit) 
	{
	case 1:
		parameters.StopBits = ONESTOPBIT;
		break;
	case 2:
		parameters.StopBits = TWOSTOPBITS;
		break;
	case 3:
		parameters.StopBits = ONE5STOPBITS; 
		break;
	}
	if (!SetCommState(serialportHandle, &parameters))
	{
		printf("[WzSerialportPlus::open()]: open failed with cannot set parameters!\n");
		CloseHandle(serialportHandle);
		return false;
	}

	/* timeouts config */
	COMMTIMEOUTS timeOuts;
	timeOuts.ReadIntervalTimeout = MAXDWORD;
	timeOuts.ReadTotalTimeoutMultiplier = 0;
	timeOuts.ReadTotalTimeoutConstant = 0;
	timeOuts.WriteTotalTimeoutMultiplier = 0;
	timeOuts.WriteTotalTimeoutConstant = 0;
	SetCommTimeouts(serialportHandle, &timeOuts);

	/* clear all buffers */
	PurgeComm(serialportHandle, PURGE_TXCLEAR | PURGE_RXCLEAR);

    receivable = true;

    std::thread([&]{
        char* receiveData = new char[receiveMaxlength];
		DWORD receivedLength = 0;
		BOOL readState = false;

        while (receivable)
        {
			memset(receiveData, 0, receiveMaxlength);

			readState = ReadFile(serialportHandle,
				receiveData,
				receiveMaxlength,
				&receivedLength,
				NULL);

			if (readState && receivedLength > 0)
			{
				onReceive(receiveData, receivedLength);
				if (nullptr != receiveCallback)
				{
					receiveCallback(receiveData, receivedLength);
				}
			}

            receivedLength = 0;
			readState = false;
			
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        delete[] receiveData;
        receiveData = nullptr; 
    }).detach();

    printf("[WzSerialportPlus::open()]: open success.\n");
    return true;

#else
	/* ---------- POSIX(Mac/Linux) termios 实现 ---------- */
	// 非阻塞方式打开，避免 DCD 未接时 open 阻塞
	serialportHandle = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (serialportHandle == kInvalidSerial) {
		printf("[WzSerialportPlus::open()]: open failed on '%s' (errno=%d), maybe permission denied or device busy!\n",
			name.c_str(), errno);
		return false;
	}

	struct termios tty;
	memset(&tty, 0, sizeof(tty));
	if (tcgetattr(serialportHandle, &tty) != 0) {
		printf("[WzSerialportPlus::open()]: tcgetattr failed (errno=%d)!\n", errno);
		::close(serialportHandle);
		serialportHandle = kInvalidSerial;
		return false;
	}

	// 波特率
	speed_t spd;
	if (!baudrateToSpeed(baudrate, spd)) {
		printf("[WzSerialportPlus::open()]: unsupported baudrate %d on this platform!\n", baudrate);
		::close(serialportHandle);
		serialportHandle = kInvalidSerial;
		return false;
	}
	cfsetispeed(&tty, spd);
	cfsetospeed(&tty, spd);

	// 原始模式（等价于 Windows 端不做行处理的裸字节流）
	cfmakeraw(&tty);

	// 数据位
	tty.c_cflag &= ~CSIZE;
	switch (databit) {
		case 7:  tty.c_cflag |= CS7; break;
		case 8:  tty.c_cflag |= CS8; break;
		default: tty.c_cflag |= CS8; break;
	}

	// 校验位
	switch (paritybit) {
		case 'n': case 'N':
			tty.c_cflag &= ~PARENB;
			break;
		case 'o': case 'O':
			tty.c_cflag |= PARENB;
			tty.c_cflag |= PARODD;
			break;
		case 'e': case 'E':
			tty.c_cflag |= PARENB;
			tty.c_cflag &= ~PARODD;
			break;
		default:
			tty.c_cflag &= ~PARENB;
			break;
	}

	// 停止位
	if (stopbit == 2) tty.c_cflag |= CSTOPB;
	else              tty.c_cflag &= ~CSTOPB;

	// 本地连接 + 允许接收
	tty.c_cflag |= (CLOCAL | CREAD);

	// 非阻塞读：VMIN=0/VTIME=0，read 立即返回可用字节数
	tty.c_cc[VMIN]  = 0;
	tty.c_cc[VTIME] = 0;

	if (tcsetattr(serialportHandle, TCSANOW, &tty) != 0) {
		printf("[WzSerialportPlus::open()]: tcsetattr failed (errno=%d)!\n", errno);
		::close(serialportHandle);
		serialportHandle = kInvalidSerial;
		return false;
	}

	// 清空收发缓冲（对应 Windows 的 PurgeComm）
	tcflush(serialportHandle, TCIOFLUSH);

	receivable = true;

	std::thread([&]{
		char* receiveData = new char[receiveMaxlength];
		while (receivable) {
			memset(receiveData, 0, receiveMaxlength);
			ssize_t receivedLength = ::read(serialportHandle, receiveData, receiveMaxlength);
			if (receivedLength > 0) {
				onReceive(receiveData, static_cast<int>(receivedLength));
				if (nullptr != receiveCallback) {
					receiveCallback(receiveData, static_cast<int>(receivedLength));
				}
			}
			// 与 Windows 版一致的轮询节拍
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		delete[] receiveData;
		receiveData = nullptr;
	}).detach();

	printf("[WzSerialportPlus::open()]: open success.\n");
	return true;
#endif
}

bool WzSerialportPlus::open(const std::string& name,
                            const int& baudrate,
                            const int& stopbit,
                            const int& databit,
                            const int& paritybit)
{
    this->name = name;
    this->baudrate = baudrate;
    this->stopbit = stopbit;
    this->databit = databit;
    this->paritybit = paritybit;
    return open();
}

void WzSerialportPlus::close()
{   
    if(receivable)
    {
        receivable = false;
    }

    if(serialportHandle != kInvalidSerial)
    {
#ifdef _WIN32
        CloseHandle(serialportHandle);
#else
        ::close(serialportHandle);
#endif
        serialportHandle = kInvalidSerial;
    }
}

int WzSerialportPlus::send(char* data,int length)
{
#ifdef _WIN32
	DWORD lengthSent = -1; 

	BOOL bWriteStat = WriteFile(serialportHandle, 
		data,
		length,
		&lengthSent,
		NULL);
	if (bWriteStat)
	{
		return lengthSent;
	}
	else 
	{
		return 0;
	}
#else
	ssize_t lengthSent = ::write(serialportHandle, data, length);
	if (lengthSent > 0) {
		return static_cast<int>(lengthSent);
	}
	return 0;
#endif
}

void WzSerialportPlus::setReceiveCalback(ReceiveCallback receiveCallback)
{
    this->receiveCallback = receiveCallback;
}

void WzSerialportPlus::onReceive(char* data,int length)
{

}
