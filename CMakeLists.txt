cmake_minimum_required(VERSION 3.13 FATAL_ERROR)

project(Relativistic_Hash_Table VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Relativistic_Hash_Table)

add_executable(Relativistic_Hash_Table 
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/RCU.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/RCUHashTable.cpp

	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/RCUTypes.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/RCUApi.h

	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/RCUHashTableTypes.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/RCUHashTableCoreApi.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/RCUHashTableApi.h
	
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/AtomicSinglyLinkedListTypes.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/AtomicSinglyLinkedListApi.h

	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/AtomicDoublyLinkedListTypes
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include/AtomicDoublyLinkedListApi.h

	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/TestsAndExamples/RCUTableTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/TestsAndExamples/RCUTableTests.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/TestsAndExamples/RCUTests.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/TestsAndExamples/RCUTests.h
	${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/TestsAndExamples/TestHelper.h
)

target_include_directories(Relativistic_Hash_Table PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Relativistic_Hash_Table/LibSource/include)
