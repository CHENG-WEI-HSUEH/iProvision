#include "text_utils.hpp"

// std::min
#include <algorithm>
// std::isspace
#include <cctype>
// std::snprintf（for formatting time strings）
#include <cstdio>

// Tokenize a line of input by whitespace; 
std::vector<std::string> Tokenize(const std::string& line) {
    // The vector to hold the resulting tokens
    std::vector<std::string> tokens;
    // The string to hold the current token being built
    std::string current;
    // Whether the current character is within double quotes
    bool quoted = false;
    // Whether the current token has any content (to allow "" to be considered a valid token)
    bool any = false;
    // Iterate over each character in the input line
    for (char c : line) {
        // If the character is a double quote, toggle the quoted state
        if (c == '"') {
            // Toggle the quoted state
            quoted = !quoted;
            // Mark that the current token has content
            any = true;
        // If the character is whitespace and we are not within quotes, it signifies the end of a token
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            // If the current token has content, add it to the tokens vector
            if (any) tokens.push_back(current);
            // Clear the current token for the next one
            current.clear();
            // Reset the any flag since we are starting a new token
            any = false;
        // Other characters: add to the current token
        } else {
            // Add this character
            current += c;
            // This token has content
            any = true;
        }
    }
    // After the loop ends, the last token hasn't been saved yet; save it
    if (any) tokens.push_back(current);
    // Return all the tokens
    return tokens;
}

// Seconds → HH:MM:SS
std::string Duration(double seconds) {
    // Only take the integer part of the seconds
    const int t = static_cast<int>(seconds);
    // Buffer to hold the result string
    char buf[32];
    // Hours = t/3600, Minutes = t/60 % 60, Seconds = t % 60
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    // Convert to std::string and return
    return buf;
}

// Edit Distance (Dynamic Programming)
size_t EditDistance(const std::string& a, const std::string& b) {
    // Dynamic programming only needs to keep one row: row[j] = distance from a's first i characters to b's first j characters
    std::vector<size_t> row(b.size() + 1);
    // First row: empty string to b's first j characters requires j insertions
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    // Iterate over each character in a
    for (size_t i = 1; i <= a.size(); ++i) {
        // diag is the value from the previous row and previous column (the "upper-left" cell in the DP table)
        size_t diag = row[0];
        // The first cell of the current row corresponds to transforming a's first i characters to an empty string, which requires i deletions
        row[0] = i;
        // Iterate over each character in b
        for (size_t j = 1; j <= b.size(); ++j) {
            // First, save the value from the previous row and previous column (the "upper-left" cell in the DP table)
            const size_t up = row[j];
            // Take the minimum of three operations: deletion (from above + 1), insertion (from left + 1), substitution (from upper-left + 1 if characters are different)
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] != b[j - 1])});
            // Move right by one cell, update the upper-left cell
            diag = up;
        }
    }
    // The last cell is the distance from a to b
    return row[b.size()];
}

// Find the closest word to 'name' in 'candidates'
std::string ClosestWord(const std::vector<std::string>& candidates, const std::string& name, size_t max_distance) {
    // The closest word found so far; initially empty
    std::string best;

    // If no candidate is within max_distance, best will remain empty
    size_t best_distance = max_distance + 1;
    // Compare each candidate word to the target name
    for (const std::string& candidate : candidates) {
        // Calculate the edit distance between the candidate and the target name
        const size_t d = EditDistance(candidate, name);

        if (d < best_distance) {
            // Update the closest word found so far
            best = candidate;
            // Update the best distance found so far
            best_distance = d;
        }
    }
    // Return the closest word found; if none is within max_distance, best will be empty
    return best;
}
