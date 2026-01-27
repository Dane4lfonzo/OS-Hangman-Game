#include <iostream>
#include <fstream>
#include <iomanip>
#include <bits/stdc++.h>

using namespace std;

int main()
{
    string wordtoguess;
    bool gamestart = true;
    string guess;
    string guessedletters = "-----";
    string enteredletter;
    char restartgame = 'Y';
    bool newgame = true;
    bool gamedone = false;

    string players[] = {"player1", "player2"};
    int turncount = 0;
    int rounds = 1;
    int playerscore[] = {0, 0};
    int order[] = {0, 1};

    while (gamestart)
    {
        if (newgame == true)
        {
            // Reset all variables
            rounds = 1;
            turncount = 0;
            for (int i=0; i <= 2; i++)
            {
                playerscore[i] = 0;
            }
            guessedletters = "-----";
            wordtoguess = "-----";
            newgame = false;
            gamedone = false;

            cout << "\nInput a 5 letter word." << endl;
            cin >> wordtoguess;

            while (wordtoguess.length() < 5 || wordtoguess.length() > 5)
            {
                cout << "The word to guess is too short/long. Input a 5 letter word" << endl;
                cin >> wordtoguess;
            }

            transform(wordtoguess.begin(), wordtoguess.end(), wordtoguess.begin(), ::toupper);
        }

        cout << "\nA new round begins." << endl;

        for (int i=0; i <= 4; i++)
        {
            if (guessedletters[i] != wordtoguess[i])
            {
                cout << endl << setw(22) << "Round " << rounds << endl;
                cout << "\n----------This is " << players[turncount % 2] << " turn----------" << endl << endl;
                cout << setw(16) << guessedletters[0] << " " << guessedletters[1] << " " << guessedletters[2] << " " << guessedletters[3] << " " << guessedletters[4] << endl;
                cout << setw(16 + (i*2)) << "^" << endl;

                cout << "\nInput letter: ";
                cin >> guess;
                guessedletters[i] = guess[0];
                transform(guessedletters.begin(), guessedletters.end(), guessedletters.begin(), ::toupper);
            
                if (guessedletters[i] != wordtoguess[i])
                {
                    for (int j=0; j <= 4; j++)
                    {
                        if (guessedletters[i] == wordtoguess[j])
                        {
                            guessedletters[i] = '*';
                        }
                    }
                }
                else
                {
                    playerscore[turncount % 2] += 1;
                }

                if (guessedletters[i] != '*' && guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }

                turncount += 1;
            }

            cout << "THE I COUNT " << i << endl;
            if (i == 4)
            {
                rounds += 1;
            }
        }

        if (guessedletters == wordtoguess)
        {
            cout << "\n\nYou guessed the word. Congrats!" << endl;
            cout << "\nThe word is " << wordtoguess << endl;

            for (int i = 0; i < 2 - 1; i++)
            {
                for (int j = i + 1; j < 2; j++)
                {
                    if (playerscore[order[j]] > playerscore[order[i]])
                    {
                        int temp = order[i];
                        order[i] = order[j];
                        order[j] = temp;
                    }
                }
            }

            cout << "\nFinal Scores:" << endl;;

            for (int i = 0; i < 2; i++)
            {
                cout << players[order[i]] << " : " << playerscore[order[i]] << endl;
            }

            // Declare winner
            cout << "\nWinner: " << players[order[0]] << endl;

            gamedone = true;
        }
        else if (rounds > 5)
        {
            cout << "\n\nYou didn't guess the word. Meh..." << endl;

            for (int i=0; i <= 4; i++)
            {
                if (guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }
            }

            cout << "\nThe word is " << wordtoguess << endl;

            gamedone = true;
        }

        if (gamedone)
        {
            cout << "\nWould you like another game? (Y/N)" << endl;
            cin >> restartgame;
            restartgame = toupper(restartgame);

            if (restartgame == 'Y')
            {
                newgame = true;
            }
            else
            {
                cout << "\nThanks for Playing" << endl;
                gamestart = false;
            }
        }
    }

    return 0;
}