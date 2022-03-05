#!/usr/bin/python3

import socket
import sys
import os

# device's IP address
SERVER_HOST = "0.0.0.0"
SERVER_PORT = 2006
# receive 4096 bytes each time
BUFFER_SIZE = 4096
SEPARATOR = "<SEPARATOR>"

# the name of file we want to send, make sure it exists
filename = sys.argv[1]
# get the file size
filesize = os.path.getsize(filename)

s = socket.socket()
s.bind((SERVER_HOST, SERVER_PORT))

s.listen(5)
print(f"[*] Listening as {SERVER_HOST}:{SERVER_PORT}")

# accept connection if there is any
client_socket, address = s.accept() 
# if below code is executed, that means the sender is connected
print(f"[+] {address} is connected.")

# start sending the file
with open(filename, "rb") as f:
    while True:
        # read the bytes from the file
        bytes_read = f.read(BUFFER_SIZE)
        if not bytes_read:
            # file transmitting is done
            break
        # we use sendall to assure transimission in 
        # busy networks
        s.sendall(bytes_read)

# close the socket
s.close()
