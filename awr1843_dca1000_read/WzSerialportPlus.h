/**
 * @file WzSerialportPlus.h
 * @author ouyangwei
 * @brief library for serial port in linux
 * @version 0.1
 * @date 2020-06-15
 * @copyright Copyright (c) 2020
 */

#ifndef __WZSERIALPORTPLUS_H__
#define __WZSERIALPORTPLUS_H__

#include <iostream>
#include <string>
#include <thread>
#include <functional>

#ifdef _WIN32
#include <WinSock2.h>
#include <windows.h>
// Windows 下串口句柄为 HANDLE
using SerialHandle = HANDLE;
#else
// POSIX(Mac/Linux) 下串口是普通文件描述符 int
using SerialHandle = int;
#endif

using ReceiveCallback = std::function<void (char*,int)>;

class WzSerialportPlus
{
public:
    WzSerialportPlus();
    /**
     * @param name: serialport name , such as: /dev/ttyS1
     * @param baudrate: baud rate , valid: 2400,4800,9600,19200,38400,57600,115200,230400
     * @param stopbit: stop bit , valid: 1,2
     * @param databit: data bit , valid: 7,8
     * @param paritybit: parity bit , valid: 'o'/'O' for odd,'e'/'E' for even,'n'/'N' for none
     */
    WzSerialportPlus(const std::string& name,
                    const int& baudrate,
                    const int& stopbit,
                    const int& databit,
                    const int& paritybit);
    ~WzSerialportPlus();

    bool open();

    /**
     * @brief parameters same as constructor
     */
    bool open(const std::string& name,
                    const int& baudrate,
                    const int& stopbit,
                    const int& databit,
                    const int& paritybit);

    void close();

    int send(char* data,int length);

    void setReceiveCalback(ReceiveCallback receiveCallback);

protected:
    virtual void onReceive(char* data,int length);

private:
	SerialHandle serialportHandle;
    std::string name;
    int baudrate;
    int stopbit;
    int databit;
    int paritybit;
    bool receivable;
    int receiveMaxlength;
    int receiveTimeout; /* unit: ms */
    ReceiveCallback receiveCallback;
};

#endif