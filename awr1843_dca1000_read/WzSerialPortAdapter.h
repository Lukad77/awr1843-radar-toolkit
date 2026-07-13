#pragma once
// WzSerialPortAdapter.h
#ifndef WZ_SERIAL_PORT_ADAPTER_H
#define WZ_SERIAL_PORT_ADAPTER_H

#include "SerialPort.h"
#include "WzSerialportPlus.h"

// 派生类：适配WzSerialportPlus库
class WzSerialPortAdapter : public SerialPort {
private:
    WzSerialportPlus port;  // 封装原库实例

public:
    // 打开串口：调用原库的open方法
    bool open(const std::string& portName, int baudRate,
        int stopBits = 1, int dataBits = 8, char parity = 'N') override {
        return port.open(portName.c_str(), baudRate, stopBits, dataBits, parity);
    }

    // 关闭串口：调用原库的close方法
    void close() override {
        port.close();
    }

    // 发送数据：处理const char*到char*的转换（核心适配点）
    int send(const char* data, int length) override {
        // 关键：使用const_cast移除const属性（前提是原库send方法不修改数据）
        return port.send(const_cast<char*>(data), length);
    }

    // 关键：转发两参数的std::function给原库
    void setReceiveCallback(ReceiveCallback callback) override {
        // 原库的setReceiveCalback接受std::function<void(char*, int)>，直接传递即可
        port.setReceiveCalback(callback);
    }
};

#endif // WZ_SERIAL_PORT_ADAPTER_H