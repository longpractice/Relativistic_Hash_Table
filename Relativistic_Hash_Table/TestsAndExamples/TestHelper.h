#include <chrono>
#include <iostream>
#include <string>

namespace yrcu
{
struct Timer
{
	Timer(const std::string& str = "untitled") : reportTitle{ str }
	{
		timeStart = std::chrono::system_clock::now();
	}

	~Timer()
	{
		if (!reported)
			report();
	}

	void report()
	{
		auto timeFinish = std::chrono::system_clock::now();
		auto diff = timeFinish - timeStart;
		std::cout << reportTitle << ": "
							<< std::chrono::duration_cast<std::chrono::milliseconds>(diff).count() << "_ms\n";
		reported = true;
	}
	std::chrono::system_clock::time_point timeStart;
	std::string reportTitle = "";
	bool reported = false;
};
}	 // namespace yrcu