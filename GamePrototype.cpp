#include <iostream>
#include <fstream>

using namespace std;

int main()
{
    string wordtoguess;
    bool gamestart = true;
    string enteredletters = "-----";

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
            if (enteredletters[i] != wordtoguess[i])
            {
                cout << "\nInput letter at Place " << i+1 << " :";
                cin >> enteredletters[i];
            
                if (enteredletters[i] != wordtoguess[i])
                {
                    for (int j=0; j <= 4; j++)
                    {
                        if (enteredletters[i] == wordtoguess[j])
                        {
                            enteredletters[i] = '*';
                        }
                    }
                }

                if (enteredletters[i] != '*' && enteredletters[i] != wordtoguess[i])
                {
                    enteredletters[i] = '_';
                }
            }
        }

        if (enteredletters == wordtoguess)
        {
            cout << "\n\nYou guessed the word. Congrats!" << endl;
            gamestart = false;
        }
        else
        {
            cout << "\n\nYou didn't guess the word. Meh..." << endl;
            cout << "\nYou guessed " << enteredletters[0] << " " << enteredletters[1] << " " << enteredletters[2] << " " << enteredletters[3] << " " << enteredletters[4] << endl << endl;

            for (int i=0; i <= 4; i++)
            {
                if (enteredletters[i] != wordtoguess[i])
                {
                    enteredletters[i] = '_';
                }
            }
        }


    }
}