#include <iostream>
#include <fstream>
#include <regex>
#include <string>


#if DEBUG
#define DEBUG_PRINT(x) /*std::cout << x << std::endl;*/
#else
#define DEBUG_PRINT(x)
#endif

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file> <regex_option (0 or 1)>\n";
        return 1;
    }

    DEBUG_PRINT("Debug: Starting program with arguments:");
    DEBUG_PRINT("  Input file: " << argv[1]);
    DEBUG_PRINT("  Output file: " << argv[2]);
    DEBUG_PRINT("  Regex option: " << argv[3]);

    std::ifstream inputFile(argv[1]);
    if (!inputFile) {
        std::cerr << "Error: Could not open input file " << argv[1] << "\n";
        return 1;
    } else {
        DEBUG_PRINT("Debug: Successfully opened input file " << argv[1]);
    }

    std::ofstream outputFile(argv[2]);
    if (!outputFile) {
        std::cerr << "Error: Could not open output file " << argv[2] << "\n";
        return 1;
    } else {
        DEBUG_PRINT("Debug: Successfully opened output file " << argv[2]);
    }

    int regexOption = std::stoi(argv[3]);
    if (regexOption != 0 && regexOption != 1) {
        std::cerr << "Error: Invalid regex option. Must be 0 or 1.\n";
        return 1;
    } else {
        DEBUG_PRINT("Debug: Regex option is valid: " << regexOption);
    }

    std::string line;
    try {
        std::regex evaluateRegex(R"(evaluate::[a-zA-Z0-9_]+)");
        std::regex emptyBraceRegex(R"(\{""\})");

        while (std::getline(inputFile, line)) {
            DEBUG_PRINT("Debug: Read line: " << line);
            if (regexOption == 0) {
                line = std::regex_replace(line, evaluateRegex, "NULL");
                DEBUG_PRINT("Debug: Applied evaluateRegex, resulting line: " << line);
            } else if (regexOption == 1) {
                line = std::regex_replace(line, emptyBraceRegex, "{\"\",0}");
                DEBUG_PRINT("Debug: Applied emptyBraceRegex, resulting line: " << line);
            }
            outputFile << line << "\n";
            DEBUG_PRINT("Debug: Wrote line to output file: " << line);
        }
    } catch (const std::regex_error& e) {
        std::cerr << "Regex error: " << e.what() << "\n";
        return 1;
    }

    inputFile.close();
    DEBUG_PRINT("Debug: Closed input file " << argv[1]);
    outputFile.close();
    DEBUG_PRINT("Debug: Closed output file " << argv[2]);

    return 0;
}
