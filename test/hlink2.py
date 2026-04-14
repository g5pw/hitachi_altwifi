#!/bin/env python3
# Reverse engineering of Hitachi HLINK2 protocol

import base64
import datetime
import json
import logging
import os
import pathlib
import re

logging.basicConfig(level=logging.ERROR, format='%(name)s: %(message)s')
log = logging.getLogger('hlink2')

TEST_RAW_RES = '.{{"TtIN":1,"MsgN":1,"DataN":1}{"Res":0}{"ChS":64815}}.'
TEST_RAW_CMD = '.{{"TtIN":1,"MsgN":1,"DataN":2}{"CMD_IDU":{"WtmS":0,"Buzz":1}}{"ChS":63165}}.'


class Line(dict):
    _raw_re = re.compile(r'^(?:(?P<timestamp>\d{2}:\d{2}:\d{2}\.\d+) >>> )?\.*{(?P<data>(?P<header>{.+})(?P<payload>{.+})(?P<checksum>{.+}))}\.*$')
    flags = {'STS_IDU', 'STS_ODU', 'STS_ALL', 'STS_REQ', 'STS_WM', 'CMD_IDU', 'CMD_ODU', 'CMD_ALL', 'CMD_UG', 'CMD_WM', 'OTA_REQ', 'CTRL_REQ'}
    patterns = dict()
    items = dict()

    def __init__(self, **kwargs):
        dict.__init__(self, dict(origin='custom',
                                 timestamp=datetime.time(0, 0, 0, 0),
                                 header={"TtIN": 1, "MsgN": 1, "DataN": 0},
                                 flag=None,
                                 values={},
                                 response=None))
        self.update(kwargs)

        if 'raw' not in self:  # custom line
            self.header["DataN"] = self.calculateDataN()

        if len(self.values) > 0:
            if (flag := f'{self["origin"][:2]}_{self.flag}') not in Line.patterns:  # retrieve TX/RX from 'origin' value
                Line.patterns[flag] = set()
                Line.items[flag] = dict()
            Line.patterns[flag].add(tuple(self.values.keys()))
            for key in self.values:
                if key not in Line.items[flag]:
                    Line.items[flag][key] = set()
                Line.items[flag][key].add(self.values[key])

    @classmethod
    def fromRaw(cls, raw, **kwargs) -> 'Line | None':
        """return a Line from raw data"""
        if match := cls._raw_re.match(raw):
            try:
                if match.group('timestamp'):
                    timestamp = datetime.datetime.strptime(match.group('timestamp'), "%H:%M:%S.%f").time()
                else:
                    timestamp = datetime.time(0, 0, 0, 0)

                payload = json.loads(match.group('payload'))
                if (response := payload.get('Res', None)) is not None:
                    payload.pop('Res')

                if len(payload) == 0:
                    flag, values = None, {}
                elif len(payload) == 1:
                    for key in payload:
                        flag = key
                        values = payload[flag]
                else:
                    log.error(f'value has multiple mappings ({payload})')
                    flag, values = '', {}

                data = dict(timestamp=timestamp,
                            header=json.loads(match.group('header')),
                            response=response,
                            flag=flag,
                            values=values,
                            checksum=json.loads(match.group('checksum'))['ChS'],
                            raw=dict(
                                header=match.group('header'),
                                value=match.group('payload'),
                                checksum=match.group('checksum'),
                            ))
                data.update(kwargs)
                return cls(**data)
            except json.JSONDecodeError as error:
                log.error(f'JSON error while reading {raw}: {error}')
        log.error(f'invalid line {raw}')
        return None

    @staticmethod
    def formatHeaders(format_spec=None) -> str:
        """return format headers (for csv)"""
        format_spec = format_spec if format_spec else ''
        return format_spec.join(('origin', 'timestamp', 'header_TtIN', 'header_MsgN', 'header_DataN', 'checksum', 'response', 'flag', 'values'))

    def __format__(self, format_spec=None):
        format_spec = format_spec if format_spec else ''
        values = [str(self['origin']),
                  self.timestamp.strftime('%H:%M:%S.%f'),
                  str(self.header['TtIN']),
                  str(self.header['MsgN']),
                  str(self.header['DataN']),
                  str(self.checksum),
                  str(self.response),
                  str(self.flag),
                  ]
        for key in self.values:
            values.append(str(key))
            values.append(str(self.values[key]))

        return format_spec.join(values)

    def calculateDataN(self) -> int:
        """return DataN value (sum of all value arguments)"""
        total = 0
        if self.flag is not None:
            total += len(self.values)
        if self.response is not None:
            total += 1
        return total

    @staticmethod
    def calculateChecksum(raw) -> int:
        """return calculated checksum from raw data"""
        total = 0xFFFF
        for i in raw:
            total -= ord(i)
        return total

    @property
    def checksum(self) -> int:
        if 'checksum' not in self:
            self['checksum'] = Line.calculateChecksum(self.raw['value'])
        return self['checksum']

    @property
    def header(self):
        return self['header']

    @property
    def timestamp(self) -> datetime.time:
        return self['timestamp']

    @property
    def flag(self):
        return self['flag']

    @property
    def values(self):
        return self['values']

    @property
    def response(self):
        return self['response']

    @property
    def raw(self):
        """return raw data"""
        values = dict()
        if self.response is not None:
            values['Res'] = self.response
        if self.flag is not None:
            values[self.flag] = self.values
        raw = dict(header=json.dumps(self.header, separators=(',', ':')),
                   value=json.dumps(values, separators=(',', ':')),)
        raw['checksum'] = checksum=json.dumps({"ChS": Line.calculateChecksum(raw['value'])}, separators=(',', ':'))
        return raw


def getLinesFromStr(raw: str, filename='') -> list[Line]:
    """Extract Lines from a string"""
    lines = []

    for raw in raw.split('\n'):
        if line := Line.fromRaw(raw, origin=filename):
            lines.append(line)
        else:
            log.warning(f'invalid line ({raw})')

    return lines


def getLinesFromFiles(path: pathlib.Path | str) -> list[Line]:
    path = pathlib.Path(path)
    lines = []

    if path.is_dir():
        log.info(f'reading files of {path}')
        for filename in next(path.walk())[-1]:
            if filename.startswith('.'):
                continue
            with open(filepath := path.joinpath(filename)) as file:
                log.info(f'reading {filepath}')
                lines += getLinesFromStr(file.read(), filepath.stem)
    elif path.is_file():
        with open(path) as file:
            log.info(f'reading {path}')
            lines += getLinesFromStr(file.read(), path.stem)

    return lines
