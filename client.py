import socket
import struct

def view_orderbook():

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    

    s.sendall(b"VIEW\n")
    
    
    header_data = s.recv(9)
    if len(header_data) < 9:
        print("Failed to receive header.")
        return
        
    msg_type, num_asks, num_bids = struct.unpack('<cII', header_data)
    

    print("\nMARKET DATA")
    print("  ASKS (Sellers)")
    

    for _ in range(num_asks):
        data = s.recv(12)
        price, qty = struct.unpack('<QI', data)
        print(f"  {price} ticks | Qty: {qty}")
        
    print("---------------------------------")
    print("  BIDS (Buyers)")
    

    for _ in range(num_bids):
        data = s.recv(12)
        price, qty = struct.unpack('<QI', data)
        print(f"  {price} ticks | Qty: {qty}")
        

    s.close()

if __name__ == "__main__":
    view_orderbook()