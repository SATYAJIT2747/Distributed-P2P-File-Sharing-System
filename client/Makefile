CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall -I.
LIBS = -pthread -lcrypto

SRCS = main.cpp \
       downloader/download_manager.cpp \
       downloader/peer_downloader.cpp \
       seeder/seeder.cpp \
       utils/crypto_utils.cpp \
       utils/client_utils.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = client

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
