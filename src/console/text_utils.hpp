#ifndef TEXT_UTILS_HPP
#define TEXT_UTILS_HPP

// size_t
#include <cstddef>
// std::string
#include <string>
// std::vector
#include <vector>

// Split a line of input by whitespace; parts enclosed in "double quotes" are treated as single tokens
std::vector<std::string> Tokenize(const std::string& line);

// Convert seconds to a string in the format HH:MM:SS, e.g., 3725 seconds → "01:02:05"
std::string Duration(double seconds);

// Calculate the edit distance (Levenshtein distance) between two strings a and b, which is the minimum number of single-character edits (insertions, deletions, or substitutions) required to change one string into the other.
size_t EditDistance(const std::string& a, const std::string& b);

// Find the closest word to 'name' in 'candidates'; if the distance exceeds max_distance, return an empty string
std::string ClosestWord(const std::vector<std::string>& candidates, const std::string& name, size_t max_distance = 2);

#endif  // TEXT_UTILS_HPP