#pragma once
// SerialPort.h
#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <string>
#include <functional>
using ReceiveCallback = std::function<void(char*, int)>;
// ������ࣺ����ͨ�ô��ڽӿ�
class SerialPort {
public:
    virtual ~SerialPort() = default;  // ������������ȷ����������ȷ����

    // �򿪴��ڣ��������˿����������ʡ�ֹͣλ������λ��У��λ��
    virtual bool open(const std::string& portName, int baudRate,
        int stopBits = 1, int dataBits = 8, char parity = 'N') = 0;

    // �رմ���
    virtual void close() = 0;

    // �������ݣ�������const char* ���ݣ����ȣ������ط����ֽ���
    virtual int send(const char* data, int length) = 0;

    // ע����ջص����������ص��������û����ݣ�
    // �������ص��������Ͱ���userData����
    virtual void setReceiveCallback(ReceiveCallback callback) = 0;
};

#endif // SERIAL_PORT_H