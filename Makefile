TARGET=rk3399mpp
CPP=aarch64-linux-gnu-g++

all:
	$(CPP) -c -g camerareader.cpp -I/home/firefly/Desktop/mpp/inc  
	$(CPP) -c -g main.cpp -I/home/firefly/Desktop/mpp/inc 
	$(CPP) -g -o $(TARGET) main.o camerareader.o -L/home/firefly/Desktop/mpp/build/linux/aarch64/mpp -lrockchip_mpp -lpthread
	
clean:
	-rm -rf *.o $TARGET
