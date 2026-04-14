#!/bin/env python3
# Basic hlink2 UART simulator

import time

# python3 -m pip install pyserial
import serial
import serial.rfc2217

import hlink2

ADDRESS = '127.0.0.1'
PORT = 4321
BAUDRATE = 9600

HEADER = '[D][uart_debug:158]: >>> "'

VALUES = {
    "Tr": 19.5,  # indoor temperature
    "Ta": 6.2,  # outdoor temperature
    "Hr": 63,  # indoor humidity
    "OnOf": 1,  # climate power state
    "Mode": 33024,  # climate requested mode
    "RlMd": 33040,  # climate actual mode
    "SetT": 20.0,  # set target temperature
    "FanS": 2,  # fan speed
    "FanSw": 1,  # fan swing mode
    "Modl": 'MODEL_NAME',
    "A11": "null", # unknown mnemonic
    "Opt1": 0,
    "Opt2": 0,
    "Opt3": 0,
    "Opt4": 1,
}


def client(address, port, baudrate) -> bool:
    try:
        connection = serial.rfc2217.Serial(
            port=f"rfc2217://{address}:{port}",
            baudrate=baudrate,
            timeout=1,
            rtscts=True,
            dsrdtr=True
        )
    except serial.serialutil.SerialException:
        return False
    else:
        print('Connected')

    connection.setDTR(False)
    time.sleep(0.1)
    connection.setDTR(True)
    time.sleep(0.5)

    try:
        line = ''
        while True:
            if data := connection.readline():
                line += filter_data(data.decode('unicode-escape', errors='ignore'))
                if response := process(line):
                    print(f'>>>: {response}')
                    connection.write(f'{response}'.encode())
                    connection.flush()
                    connection.write(b'\r\n')
                    line = ''
                elif response == '':
                    line = ''
    except (KeyboardInterrupt, serial.SerialException):
        pass
    finally:
        print('Disconnected')
        connection.close()
    return True


def filter_data(data: str, header=HEADER):
    if index_header := data.find(header) > 0:
        return data[(index_header + len(header) + 6):-7]
    return ''


def status_response(request: hlink2.Line) -> str:
    response = hlink2.Line(
        flag=request.flag,
        values={m: VALUES.get(m, '0') for m in request.values.values()},
        response=0,
    )
    return f'\x02{response.raw["header"]}{response.raw["value"]}{response.raw["checksum"]}\x03\x00'


def command_response(request: hlink2.Line) -> str:
    for m, value in request.values.items():
        VALUES[m] = value
        print(f'set Mnemonic {m}: {value}')

    response = hlink2.Line(
        flag=request.flag,
        values={},
        response=0,
    )
    return f'\x02{response.raw["header"]}{response.raw["value"]}{response.raw["checksum"]}\x03'


def process(line: str) -> str | None:
    if len(line) > 3 and line[0] == '\x02' and line[-1] == '\x03':
        if '\x03' in line:
            if request := hlink2.Line.fromRaw(line[1:line.find('\x03')]):
                print(f'<<<: {line[1:line.find("\x03")]}')
                if request.flag in ('STS_IDU', 'STS_ODU'):
                    return status_response(request)
                elif request.flag == 'CMD_IDU':
                    return command_response(request)
            return ''
    return None


if __name__ == '__main__':
    while True:
        while not client(ADDRESS, PORT, BAUDRATE):
            time.sleep(5)
