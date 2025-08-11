# Windows-only Makefile for MinGW
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -static-libgcc -static-libstdc++
LIBS = -lcurl -lws2_32 -lwldap32 -lcrypt32 -lbcrypt -pthread
TARGET = youtube_downloader.exe
SOURCE = youtube_downloader.cpp
WINDOWS_FLAGS = -DWIN32_LEAN_AND_MEAN

all: $(TARGET)

$(TARGET): $(SOURCE)
	@echo "Building Windows executable..."
	$(CXX) $(CXXFLAGS) $(WINDOWS_FLAGS) -o $(TARGET) $(SOURCE) $(LIBS)
	@echo "Build completed: $(TARGET)"

clean:
	rm -f $(TARGET) *.tmp *.part*

debug: CXXFLAGS += -g -DDEBUG
debug: $(TARGET)

release: CXXFLAGS += -O3 -DNDEBUG -s
release: $(TARGET)

.PHONY: all clean debug release