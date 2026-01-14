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

    string players[] = {"player1", "player2", "player3"};
    int turncount = 0;
    int playerscore[] = {0, 0, 0};
    int order[] = {0, 1, 2};

    cout << "Input a 5 letter word." << endl;
    cin >> wordtoguess;

    while (wordtoguess.length() < 5 || wordtoguess.length() > 5)
    {
        cout << "The word to guess is too short/long. Input a 5 letter word" << endl;
        cin >> wordtoguess;
    }
    
    transform(wordtoguess.begin(), wordtoguess.end(), wordtoguess.begin(), ::toupper);

    while (gamestart)
    {
        cout << "You are the player. Input a letter in the blank to fill in the word." << endl;

        for (int i=0; i <= 4; i++)
        {
            if (guessedletters[i] != wordtoguess[i])
            {
                cout << "\n----------This is " << players[turncount % 3] << " turn----------" << endl << endl;
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
                    playerscore[turncount % 3] += 1;
                }

                if (guessedletters[i] != '*' && guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }

                turncount += 1;
            }

        }

        if (guessedletters == wordtoguess)
        {
            cout << "\n\nYou guessed the word. Congrats!" << endl;
            cout << "\nThe word is " << wordtoguess << endl;

            for (int i = 0; i < 3 - 1; i++)
            {
                for (int j = i + 1; j < 3; j++)
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

            for (int i = 0; i < 3; i++)
            {
                cout << players[order[i]] << " : " << playerscore[order[i]] << endl;
            }

            // check if top score is tied
            if (playerscore[order[0]] == playerscore[order[1]])
            {
                cout << "\nWinner: ";

                cout << players[order[0]];

                for (int i = 1; i < 3; i++)
                {
                    if (playerscore[order[i]] == playerscore[order[0]])
                    {
                        cout << " and " << players[order[i]];
                    }
                }

                cout << " (tie)" << endl;
            }
            else
            {
                cout << "\nWinner: " << players[order[0]] << endl;
            }


            gamestart = false;
        }
        else
        {
            cout << "\n\nYou didn't guess the word. Meh..." << endl;
            //cout << "\nYou guessed " << guessedletters[0] << " " << guessedletters[1] << " " << guessedletters[2] << " " << guessedletters[3] << " " << guessedletters[4] << endl << endl;

            for (int i=0; i <= 4; i++)
            {
                if (guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }
            }
        }

        


    }

    return 0;
}