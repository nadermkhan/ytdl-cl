# Makefile for Windows MinGW
CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2 -static-libgcc -static-libstdc++
LIBS = -lcurl -lws2_32 -lwldap32 -lcrypt32 -lbcrypt -pthread
TARGET = youtube_downloader.exe
SOURCE = youtube_downloader.cpp

# Windows-specific flags
WINDOWS_FLAGS = -DCURL_STATICLIB -DWIN32_LEAN_AND_MEAN

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(WINDOWS_FLAGS) -o $(TARGET) $(SOURCE) $(LIBS)

clean:
	del /Q $(TARGET) *.tmp *.part* 2>nul || true

.PHONY: all clean