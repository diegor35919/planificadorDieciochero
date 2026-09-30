CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17
TARGET   := planificador
SRCS     := main.cpp parser.cpp scheduler.cpp process_manager.cpp
OBJS     := $(SRCS:.cpp=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Dependencias de headers
main.o:            main.cpp parser.hpp scheduler.hpp process_manager.hpp
parser.o:          parser.cpp parser.hpp
scheduler.o:       scheduler.cpp scheduler.hpp parser.hpp
process_manager.o: process_manager.cpp process_manager.hpp scheduler.hpp parser.hpp

clean:
	rm -f $(OBJS) $(TARGET)

# Ejecución de prueba: ./planificador plan.txt 3
run: all
	./$(TARGET) plan.txt 3
