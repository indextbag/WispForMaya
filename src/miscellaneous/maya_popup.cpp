#include "maya_popup.hpp"

// Maya includesd
#include <maya/MGlobal.h>
#include <maya/MString.h>

// STD includes
#include <fstream>
#include <sstream>


namespace wmr
{
	void MayaPopup::Spawn(std::stringstream &content, const Options& options) noexcept
	{
		constexpr const char const* text_prefix = "text - ww on - align \"left\" - rs on - w 400 \"";
		constexpr const char const* text_postfix = "\";";

		// Create window
		MString notify_command("window -title \"Wisp\" -sizeable off -maximizeButton off -minimizeButton off WispInfoWindow;\n");

		// Set layout
		notify_command += "rowColumnLayout -columnOffset 1 \"both\" 10 -rowOffset 1 \"both\" 15 -nc 1 -cal 1 \"left\";\n";

		// Print text
		std::string line;
		while (std::getline(content, line))
		{
			// Add a space if an empty line was found
			if (line.length() <= 0)
			{
				line += " ";
			}

			// Text settings
			notify_command += text_prefix;
			// Add text
			notify_command += line.c_str();
			// Add end quote
			notify_command += text_postfix;
		}

		// Add empty line for proper spacing
		notify_command += text_prefix;
		notify_command += " ";
		notify_command += text_postfix;

		// Add button to close
		if (options.btn_ok)
		{
			notify_command += "button - enable on - command \"deleteUI WispInfoWindow\" \"Ok\";\n";

			// Add spacing below the button
			notify_command += text_prefix;
			notify_command += " ";
			notify_command += text_postfix;
		}

		// Display window
		notify_command += "showWindow WispInfoWindow;";

		MGlobal::displayInfo(notify_command);

		// Execute display window command
		MGlobal::executeCommand(notify_command);
	}

	bool MayaPopup::SpawnFromFile(const char* path, const Options& options) noexcept
	{
		// Get file
		std::ifstream infile(path);
		if (!infile.is_open())
		{
			std::stringstream s;
			s << infile.rdbuf();
			infile.close();

			MayaPopup::Spawn(s, options);
			return true;
		}
		return false;
	}
}
