#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "sim_bp.h"

// Global variables for predictors
std::vector<int> bimodalTable;
std::vector<int> gshareTable;
std::vector<int> chooserTable;
int numBimodalEntries = 0, numGshareEntries = 0, numChooserEntries = 0;
int predictions = 0, mispredictions = 0;
unsigned int globalHistoryRegister = 0;
int m1 = 0, n = 0, m2 = 0, k = 0;  // Predictor parameters for hybrid
bool isBimodal = false, isGshare = false, isHybrid = false;

// Function to get bimodal index
int getBimodalIndex(unsigned long int addr) {
    return (addr >> 2) & (numBimodalEntries - 1);  // Use m2 bits, ignoring the lowest two bits
}

// Function to get gshare index
int getGshareIndex(unsigned long int addr) {
    int pcIndex = (addr >> 2) & ((1 << m1) - 1);  // Use m1 bits, ignoring the lowest two bits
    int upperBitsOfPC = (pcIndex >> (m1 - n)) & ((1 << n) - 1);  // Take the uppermost n bits
    int gshareIndex = (upperBitsOfPC ^ (globalHistoryRegister & ((1 << n) - 1))) << (m1 - n);
    gshareIndex |= (pcIndex & ((1 << (m1 - n)) - 1));  // Combine with remaining m1-n bits
    return gshareIndex;
}

// Function to get chooser index, using bits k+1 to bit 2 of the PC
int getChooserIndex(unsigned long int addr) {
    return (addr >> 2) & ((1 << k) - 1);  // Use k bits starting from bit 2
}

// Prediction functions
bool makeBimodalPrediction(int index) {
    return bimodalTable[index] >= 2;  // Predict "taken" if counter is 2 or 3
}

bool makeGsharePrediction(int index) {
    return gshareTable[index] >= 2;  // Predict "taken" if counter is 2 or 3
}

// Update functions for prediction tables
void updatePredictor(std::vector<int>& table, int index, bool outcome) {
    if (outcome) {  // Branch actually taken
        if (table[index] < 3) table[index]++;  // Increment counter, max value is 3
    } else {        // Branch actually not taken
        if (table[index] > 0) table[index]--;  // Decrement counter, min value is 0
    }
}

// Function to update the global branch history register for gshare
void updateGlobalHistoryRegister(bool outcome) {
    globalHistoryRegister = (globalHistoryRegister >> 1) | (outcome << (n - 1));
}

// Update chooser counter based on accuracy of gshare and bimodal predictions
void updateChooser(int chooserIndex, bool gshareCorrect, bool bimodalCorrect) {
    if (gshareCorrect && !bimodalCorrect) {
        if (chooserTable[chooserIndex] < 3) chooserTable[chooserIndex]++;  // Favor gshare, saturate at 3
    } else if (bimodalCorrect && !gshareCorrect) {
        if (chooserTable[chooserIndex] > 0) chooserTable[chooserIndex]--;  // Favor bimodal, saturate at 0
    }
    // No change if both predictors are correct or both are incorrect
}

int main(int argc, char* argv[]) {
    FILE *FP;
    char *trace_file;
    bp_params params;
    char outcome;
    unsigned long int addr;

    // Determine the predictor type based on arguments
    if (argc == 4 && strcmp(argv[1], "bimodal") == 0) {
        // Bimodal mode
        isBimodal = true;
        m2 = strtoul(argv[2], NULL, 10);
        trace_file = argv[3];
        printf("COMMAND\n%s %s %d %s\n", argv[0], argv[1], m2, trace_file);

        // Initialize bimodal table
        numBimodalEntries = 1 << m2;
        bimodalTable = std::vector<int>(numBimodalEntries, 2);  // Initialize to "weakly taken" (2)
    } 
    else if (argc == 5 && strcmp(argv[1], "gshare") == 0) {
        // Gshare mode
        isGshare = true;
        m1 = strtoul(argv[2], NULL, 10);
        n = strtoul(argv[3], NULL, 10);
        trace_file = argv[4];
        printf("COMMAND\n%s %s %d %d %s\n", argv[0], argv[1], m1, n, trace_file);

        // Initialize gshare table
        numGshareEntries = 1 << m1;
        gshareTable = std::vector<int>(numGshareEntries, 2);  // Initialize to "weakly taken" (2)
        globalHistoryRegister = 0;  // Initialize global history register to zero
    } 
    else if (argc == 7 && strcmp(argv[1], "hybrid") == 0) {
        // Hybrid mode
        isHybrid = true;
        k = strtoul(argv[2], NULL, 10);   // Chooser table bits
        m1 = strtoul(argv[3], NULL, 10);  // Gshare predictor bits
        n = strtoul(argv[4], NULL, 10);   // Global history bits
        m2 = strtoul(argv[5], NULL, 10);  // Bimodal predictor bits
        trace_file = argv[6];
        printf("COMMAND\n%s %s %d %d %d %d %s\n", argv[0], argv[1], k, m1, n, m2, trace_file);

        // Initialize tables for hybrid predictor
        numBimodalEntries = 1 << m2;
        numGshareEntries = 1 << m1;
        numChooserEntries = 1 << k;

        bimodalTable = std::vector<int>(numBimodalEntries, 2);  // Initialize to "weakly taken" (2)
        gshareTable = std::vector<int>(numGshareEntries, 2);    // Initialize to "weakly taken" (2)
        chooserTable = std::vector<int>(numChooserEntries, 1);  // Initialize chooser to 1 
        globalHistoryRegister = 0;  // Initialize global history register to zero
    } else {
        printf("Error: Unsupported branch predictor configuration\n");
        exit(EXIT_FAILURE);
    }

    // Open trace file
    FP = fopen(trace_file, "r");
    if (FP == NULL) {
        printf("Error: Unable to open file %s\n", trace_file);
        exit(EXIT_FAILURE);
    }

    char str[2];
    while (fscanf(FP, "%lx %s", &addr, str) != EOF) {
        outcome = str[0];
        bool actualOutcome = (outcome == 't');  // True if taken, false if not taken

        // Predictions based on predictor type
        int chooserIndex = 0, bimodalIndex = 0, gshareIndex = 0;
        bool prediction = false;

        if (isBimodal) {
            // Bimodal mode
            bimodalIndex = getBimodalIndex(addr);
            prediction = makeBimodalPrediction(bimodalIndex);
            if (prediction != actualOutcome) mispredictions++;
            updatePredictor(bimodalTable, bimodalIndex, actualOutcome);
        } 
        else if (isGshare) {
            // Gshare mode
            gshareIndex = getGshareIndex(addr);
            prediction = makeGsharePrediction(gshareIndex);
            if (prediction != actualOutcome) mispredictions++;
            updatePredictor(gshareTable, gshareIndex, actualOutcome);
            updateGlobalHistoryRegister(actualOutcome);
        } 
        else if (isHybrid) {
            // Hybrid mode
            bimodalIndex = getBimodalIndex(addr);
            gshareIndex = getGshareIndex(addr);
            chooserIndex = getChooserIndex(addr);

            bool bimodalPrediction = makeBimodalPrediction(bimodalIndex);
            bool gsharePrediction = makeGsharePrediction(gshareIndex);
            prediction = (chooserTable[chooserIndex] >= 2) ? gsharePrediction : bimodalPrediction;

            if (prediction != actualOutcome) mispredictions++;
            if (chooserTable[chooserIndex] >= 2) updatePredictor(gshareTable, gshareIndex, actualOutcome);
            else updatePredictor(bimodalTable, bimodalIndex, actualOutcome);

            updateGlobalHistoryRegister(actualOutcome);
            updateChooser(chooserIndex, (gsharePrediction == actualOutcome), (bimodalPrediction == actualOutcome));
        }

        predictions++;
    }
    fclose(FP);

    // Output results
    printf("OUTPUT\n");
    printf(" number of predictions:    %d\n", predictions);
    printf(" number of mispredictions: %d\n", mispredictions);
    printf(" misprediction rate:       %.2f%%\n", (static_cast<double>(mispredictions) / predictions) * 100);

    if (isBimodal) {
        printf("FINAL BIMODAL CONTENTS\n");
        for (int i = 0; i < numBimodalEntries; i++) {
            printf("%d\t%d\n", i, bimodalTable[i]);
        }
    } else if (isGshare) {
        printf("FINAL GSHARE CONTENTS\n");
        for (int i = 0; i < numGshareEntries; i++) {
            printf("%d\t%d\n", i, gshareTable[i]);
        }
    } else if (isHybrid) {
        printf("FINAL CHOOSER CONTENTS\n");
        for (int i = 0; i < numChooserEntries; i++) {
            printf("%d\t%d\n", i, chooserTable[i]);
        }
        printf("FINAL GSHARE CONTENTS\n");
        for (int i = 0; i < numGshareEntries; i++) {
            printf("%d\t%d\n", i, gshareTable[i]);
        }
        printf("FINAL BIMODAL CONTENTS\n");
        for (int i = 0; i < numBimodalEntries; i++) {
            printf("%d\t%d\n", i, bimodalTable[i]);
        }
    }

    return 0;
}
