#!/usr/bin/python3

import re
import serial
import time
import os,sys

class UPS2:
    def __init__(self,port):
        self.ser  = serial.Serial(port,9600)        
        
    def get_data(self,nums):
        while True:
            self.ser.reset_input_buffer()
            self.count = self.ser.inWaiting()
            
            if self.count !=0:
                self.recv = self.ser.read(nums)
                return self.recv
    
    def decode_uart(self):
        self.uart_string = self.get_data(100)
        self.data = self.uart_string.decode('ascii','ignore')
        self.pattern = r'\$ (.*?) \$'
        self.result = re.findall(self.pattern,self.data,re.S)
    
        self.tmp = self.result[0]
    
        self.pattern = r'SmartUPS (.*?),'
        self.version = re.findall(self.pattern,self.tmp)
    
        self.pattern = r',Vin (.*?),'
        self.vin = re.findall(self.pattern,self.tmp)
        
        self.pattern = r'BATCAP (.*?),'
        self.batcap = re.findall(self.pattern,self.tmp)
        
        self.pattern = r',Vout (.*)'
        self.vout = re.findall(self.pattern,self.tmp)

        return self.version[0],self.vin[0],self.batcap[0],self.vout[0]
 

batpack = UPS2("/dev/ttyAMA2")

def reflash_data():
    version,vin,batcap,vout = batpack.decode_uart()

    chg="Discharging" if vin=="NG" else "Charged" if batcap=="100" else "Charging"

    with open("UPSstat.info","w") as f:
        f.write(chg+"("+batcap+"%,"+vout+"mV)\n")
    print(chg+"("+batcap+"%,"+vout+"mV)\n")
if __name__=="__main__":
    while (1):
        reflash_data()
        time.sleep(10)
