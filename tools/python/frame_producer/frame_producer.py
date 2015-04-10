'''
Created on Jan 15, 2015 - Based upon LpdFemUdpProducer.py (author: tcn45)

@author: ckd27546
'''

import argparse
import numpy as np
import socket
import time

class FrameProducerError(Exception):
    
    def __init__(self, msg):
        self.msg = msg
        
    def __str__(self):
        return repr(self.msg)

class FrameProducer(object):
 
    # Define custom class for Percival header
    HeaderType = np.dtype([('PacketType', '>i1'), ('SubframeNumber', '>i1'), ('FrameNumber', '>i4'), ('PacketNumber', '>i2'), ('Information', '>i1', 14) ])

    def __init__(self, host, port, frames, interval, display, multihosts):
        
        self.host = host
        self.port = port
        self.frames = frames
        self.interval = interval
        self.display = display
        
        # DEBUG:
        #print "FrameProducer, arg host:", host, " port:", port, " frames:", frames, " interval:", interval, " display: ", display, "multihost: ", multihosts
        
        # Initialise shape of data arrays in terms of sections and pixels
        # 1 quarter     = 704 x 742 pixels (columns x rows)
        self.quarterRows = 2
        self.quarterCols = 2        # 2
        self.colBlocksPerQuarter = 22
        self.colsPerColBlock     = 32
        self.rowBlocksPerQuarter = 106
        self.rowsPerRowBlock     = 7
        
        self.numPixelRows   = self.quarterRows * self.rowBlocksPerQuarter * self.rowsPerRowBlock
        self.numPixelCols   = self.quarterCols * self.colBlocksPerQuarter * self.colsPerColBlock
        self.subframePixels = self.numPixelRows * self.numPixelCols / 2
        
        # Initialise an array representing a single image in the system
        self.imageArray = np.empty((self.numPixelRows * self.numPixelCols), dtype=np.uint16)
        self.resetArray = np.ones((self.numPixelRows * self.numPixelCols), dtype=np.uint16)
        
        self.numADCs = 224
        
        B0B1        = 0     # Which of the 4 horizontal regions does data come from?
        coarseValue = 0     # Which LVDS pair does data belong to?
        
        timing1 = time.time()
        # Fill in image array with data according to emulator specifications
        for subframe in xrange(2):
            B0B1 = 0

            for row in xrange(self.rowBlocksPerQuarter * self.quarterRows): # 106 * 2 = 212
                coarseValue = 0
                
                if (row % 53 == 0) and (row != 0):
                    B0B1 += 1

                for column in xrange(self.colBlocksPerQuarter):

                    # New implementation? Takes ~0.975 seconds
                    index = (subframe * self.subframePixels) + (row * 4928) + (column * 224)
                    self.imageArray[index:(index+224)] = [(coarseValue << 10) + (adc << 2) + B0B1 for adc in xrange(self.numADCs)]
                    self.resetArray[index:(index+224)] = [(coarseValue << 10) + (1 << 2)] * self.numADCs
                    
                    coarseValue += 1
        
        timing2 = time.time()
        print "Nested loops took  %.3f secs" % (timing2 - timing1)
        
        # Convert data stream to byte stream for transmission
        self.imageStream = self.imageArray.tostring()
        self.resetStream = self.resetArray.tostring()
        
        
    def run(self):
        
        self.payloadLen     = 8192
        startOfFrame        = 0
        endOfFrame          = 0
        self.bytesPerPixels = 2
        self.subframeSize   = self.subframePixels * self.bytesPerPixels

        print "Starting Percival data transmission to address", self.host, "port", self.port, "..."
                
        # Open UDP socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        
        # Create packet header
        header = np.zeros(1, dtype=self.HeaderType)
        
        # Load header with dummy values
        header['PacketType']        = 0   # Packet Type        (1 Byte)
        header['SubframeNumber']    = 0   # Subframe Number    (1 Byte)
        header['FrameNumber']       = 0   # Frame Number       (4 Bytes)    
        header['PacketNumber']      = 0   # Packet Number      (2 Bytes)
        header['Information']       = 0   # Information        (14 Bytes)
        
        totalBytesSent = 0
        runStartTime = time.time()
        
        for frame in range(self.frames):
            
            print "frame: ", frame
            
            for packetType in range(2):

                # Use imageStream if packetType = 1,  otherwise use resetStream
                bytesRemaining = len(self.imageStream) if packetType == 1 else len(self.resetStream)

                streamPosn      = 0
                subframeCounter = 0
                packetCounter   = 0
                bytesSent       = 0
                subframeTotal   = 0       # How much of current subframe has been sent
                header['PacketType'] = packetType
                
                while bytesRemaining > 0:
                    
                    # Calculate packet size and construct header
    
                    if bytesRemaining <= self.payloadLen:
                        bytesToSend = bytesRemaining
                        header['PacketNumber'] = packetCounter | endOfFrame
    
                    else:
                        
                        subframeRemainder = self.subframeSize - subframeTotal
                        # Would sending full payload contain data from next subframe?
                        if (subframeRemainder < self.payloadLen):
                            bytesToSend = subframeRemainder
                        else:
                            bytesToSend = self.payloadLen
                        header['PacketNumber'] = packetCounter | startOfFrame if packetCounter == 0 else packetCounter
    
                    header['SubframeNumber'] = subframeCounter
                    header['FrameNumber']    = (frame * 2) + packetType  
                        
                    # Prepend header to current packet
                    if packetType == 0:
                        packet = header.tostring() + self.imageStream[streamPosn:streamPosn+bytesToSend]
                    else:
                        packet = header.tostring() + self.resetStream[streamPosn:streamPosn+bytesToSend]
    
                    # Transmit packet (image, reset sent to consecutive ports)
                    bytesSent += sock.sendto(packet, (self.host, self.port + packetType))
    
                    bytesRemaining  -= bytesToSend
                    streamPosn      += bytesToSend
                    packetCounter   += 1
                    subframeTotal   += bytesToSend

                   # "Image" if packetType = 0, otherwise "Reset"
                    dataDesc = "Image" if packetType == 0 else "Reset"

                    if subframeTotal >= self.subframeSize:
                        print "  Sent", dataDesc, "frame:", frame, "subframe:", subframeCounter, "packets:", packetCounter, "bytes:", bytesSent
                        subframeTotal   = 0
                        subframeCounter += 1
                        packetCounter   = 0
                        totalBytesSent  += bytesSent
                        bytesSent       = 0


        runTime = time.time() - runStartTime

        # Close socket
        sock.close()

        print "%d frames completed, %d bytes sent in %.3f secs" % (self.frames, totalBytesSent, runTime)
        
        if self.display:
            self.displayImage()
            
    def displayImage(self):

        try:
            import matplotlib.pyplot as plt
        except ImportError:
            print "\n*** matplotlib not installed, cannot plot data."
        else:
            self.imageArray = np.reshape(self.imageArray, (self.numPixelRows, self.numPixelCols))
            self.resetArray = np.reshape(self.resetArray, (self.numPixelRows, self.numPixelCols))
            fig = plt.figure(1)
            ax = fig.add_subplot(121)
            img = ax.imshow(self.imageArray)
            plt.xlabel("Image Data")
            ax = fig.add_subplot(122)
            img = ax.imshow(self.resetArray)
            plt.xlabel("Reset Data")
            plt.show()
        
    
if __name__ == '__main__':

    # Define default list of destination IP addresses
    addressList = ['192.168.0.1', '192.168.1.1', '192.168.2.1', '192.168.3.1']
    parser = argparse.ArgumentParser(description="FrameProducer - generate a simulated UDP frame data stream")
    
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--host', type=str, default='127.0.0.1', 
                        help="select destination host IP address")
    parser.add_argument('--port', type=int, default=61649,
                        help='select destination host IP port')
    parser.add_argument('--frames', '-n', type=int, default= 0, #1,
                        help='select number of frames to transmit')
    parser.add_argument('--interval', '-t', type=float, default=0.1,
                        help="select frame interval in seconds")
    parser.add_argument('--display', "-d", action='store_true',
                        help="Enable diagnostic display of generated image")
    # Support single source - multiple destinations 
    group.add_argument('--multihosts', nargs='*', #default=addressList,
                        help='Define multiple destination IP addresses')
     
    args = parser.parse_args()

    # Determine whether multiple destinations selected
    if args.multihosts is not None:
        if len(args.multihosts) == 0:
            # Use defaults
            args.multihosts = addressList
    
    producer = FrameProducer(**vars(args))
    producer.run()
    # PercivalDummy
