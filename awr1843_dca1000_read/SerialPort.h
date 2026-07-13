#pragma once
// SerialPort.h
#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <string>
using ReceiveCallback = std::function<void(char*, int)>;
// 抽象基类：定义通用串口接口
class SerialPort {
public:
    virtual ~SerialPort() = default;  // 虚析构函数，确保派生类正确析构

    // 打开串口：参数（端口名、波特率、停止位、数据位、校验位）
    virtual bool open(const std::string& portName, int baudRate,
        int stopBits = 1, int dataBits = 8, char parity = 'N') = 0;

    // 关闭串口
    virtual void close() = 0;

    // 发送数据：参数（const char* 数据，长度），返回发送字节数
    virtual int send(const char* data, int length) = 0;

    // 注册接收回调：参数（回调函数，用户数据）
    // 修正：回调函数类型包含userData参数
    virtual void setReceiveCallback(ReceiveCallback callback) = 0;
};

#endif // SERIAL_PORT_H