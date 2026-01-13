#include <iostream>
#include <fstream>

using namespace std;

int main()
{
    string wordtoguess;
    bool gamestart = true;
    string guessedletters = "-----";
    string enteredletter;

    string players[] = {"player1", "player2", "player3"};
    int turncount = 0;


    cout << "Input a 5 letter word." << endl;
    cin >> wordtoguess;

    if (wordtoguess.length() < 5 || wordtoguess.length() > 5)
    {
        cout << "The word to guess is too short/long." << endl;
        return 1;
    }

    while (gamestart)
    {
        cout << "You are the player. Input a letter in the blank to fill in the word." << endl;

        for (int i=0; i <= 4; i++)
        {
            if (guessedletters[i] != wordtoguess[i])
            {
                cout << "\n----------This is " << players[turncount % 3] << " turn----------" << endl;
                cout << "\nInput letter at Place " << i+1 << " :";
                cin >> guessedletters[i];

            
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

                if (guessedletters[i] != '*' && guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }
            }

            turncount += 1;
        }

        if (guessedletters == wordtoguess)
        {
            cout << "\n\nYou guessed the word. Congrats!" << endl;
            gamestart = false;
        }
        else
        {
            cout << "\n\nYou didn't guess the word. Meh..." << endl;
            cout << "\nYou guessed " << guessedletters[0] << " " << guessedletters[1] << " " << guessedletters[2] << " " << guessedletters[3] << " " << guessedletters[4] << endl << endl;

            for (int i=0; i <= 4; i++)
            {
                if (guessedletters[i] != wordtoguess[i])
                {
                    guessedletters[i] = '_';
                }
            }
        }


    }
}