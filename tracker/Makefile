CXX = g++
CXXFLAGS = -std=c++17 -pthread -Wall -I.
LIBS = -pthread -lcrypto

SRCS = main.cpp \
       state/tracker_state.cpp \
       sync/tracker_sync.cpp \
       handler/client_handler.cpp \
       utils/tracker_utils.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = tracker

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
