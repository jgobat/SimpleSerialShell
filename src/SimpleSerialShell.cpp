#include <Arduino.h>
#include <fnmatch.h>
#include <SdFat.h>
#include <SimpleSerialShell.h>

////////////////////////////////////////////////////////////////////////////////
/*!
 *  @file SimpleSerialShell.cpp
 *
 *  Implementation for the shell.
 *
 */



SimpleSerialShell shell;

//
SimpleSerialShell::Command * SimpleSerialShell::firstCommand = NULL;

////////////////////////////////////////////////////////////////////////////////
/*!
 *  @brief associates a named command with the function to call.
 */
class SimpleSerialShell::Command {
    public:
        Command(const __FlashStringHelper * n, CommandFunction f, boolean g, const __FlashStringHelper *u):
            name(n), myFunc(f), glob(g), usage(u) {};

        int execute(int argc, char **argv)
        {
            return myFunc(argc, argv);
        };

        // to sort commands
        int compare(const Command * other) const
        {
            const String otherNameString(other->name);
            return compareName(otherNameString.c_str());
        };

        int compareName(const char * aName) const
        {
            const String myNameString(name);
            int comparison = strncasecmp(myNameString.c_str(), aName, BUFSIZE);
            return comparison;
        };

        const __FlashStringHelper * name;
        CommandFunction myFunc;
        Command * next;
        boolean glob;
        const __FlashStringHelper *usage;
};

////////////////////////////////////////////////////////////////////////////////
SimpleSerialShell::SimpleSerialShell()
    : shellConnection(NULL),
      m_lastErrNo(EXIT_SUCCESS)
{
    resetBuffer();
    addFallback(NULL);
    addStrings(NULL);
    addFloats(NULL);
    addRedirector(NULL);
    addSD(NULL);

    // simple help.
    addCommand(F("help"), SimpleSerialShell::printHelp, false, NULL);
};

char **SimpleSerialShell::glob(SdFat *sd, char *spec, int *n)
{
    SdFile root;
    SdFile entry;
    char   buff[16];
    int n_match = 0;
    char **matches = NULL;

    *n = 0;
    if (!sd) 
        return NULL;
        
    sd -> vol()->cwd(buff, 16);
    
    if (!root.open(buff)) {
        return NULL;
    }    

    while (entry.openNext(&root, O_RDONLY)) {   
        entry.getName(buff, 16);  

        if (fnmatch(spec, buff, 0) == 0) {
            matches = (char **) realloc(matches, sizeof(char *)*(n_match + 1));
            matches[n_match ++] = strndup(buff, 16);
        }
        entry.close();
    }
    root.close();

    *n = n_match;
    return matches;
}
//////////////////////////////////////////////////////////////////////////////
void SimpleSerialShell::addCommand(
    const __FlashStringHelper * name, CommandFunction f, boolean g, const __FlashStringHelper *u)
{
    auto * newCmd = new Command(name, f, g, u);

    // insert in list alphabetically
    // from stackoverflow...

    Command* temp2 = firstCommand;
    Command** temp3 = &firstCommand;
    while (temp2 != NULL && (newCmd->compare(temp2) > 0) )
    {
        temp3 = &temp2->next;
        temp2 = temp2->next;
    }
    *temp3 = newCmd;
    newCmd->next = temp2;
}

void SimpleSerialShell::addFallback(int (*fb)(int argc, char **argv))
{
    fallback = fb;
}

void SimpleSerialShell::addStrings(char *(*s)(char *argv))
{
    stringExpand = s;
}

void SimpleSerialShell::addFloats(float (*s)(char *argv))
{
    floatExpand = s;
}

void SimpleSerialShell::addRedirector(Stream *(*f)(Stream *c, SdFile *f))
{
    consoleChange = f;
}

void SimpleSerialShell::addSD(SdFat *s)
{
    sd = s;
}
//////////////////////////////////////////////////////////////////////////////
bool SimpleSerialShell::executeIfInput(void)
{
    bool bufferReady = prepInput();
    bool didSomething = false;

    if (bufferReady) {
        didSomething = true;
        execute();
    }

    return didSomething;
}

//////////////////////////////////////////////////////////////////////////////
void SimpleSerialShell::attach(Stream & requester)
{
    shellConnection = &requester;
}

//////////////////////////////////////////////////////////////////////////////
// Arduino serial monitor appears to 'cook' lines before sending them
// to output, so some of this is overkill.
//
// But for serial terminals, backspace would be useful.
//
bool SimpleSerialShell::prepInput(void)
{
    bool bufferReady = false; // assume not ready
    bool moreData = true;

    do {
        int c = read();
        switch (c)
        {
            case -1: // No character present; don't do anything.
                moreData = false;
                break;
            case  0: // throw away NUL characters
                break;

            // Line editing characters
            case 127: // DEL delete key
            case '\b':  // CTRL(H) backspace
                // Destructive backspace: remove last character
                if (inptr > 0) {
                    print("\b \b");  // remove char in raw UI
                    linebuffer[--inptr] = 0;
                }
                break;

            case 0x12: //CTRL('R')
                //Ctrl-R retypes the line
                print("\r\n");
                print(linebuffer);
                break;

            case 0x15: //CTRL('U')
                //Ctrl-U deletes the entire line and starts over.
                println("XXX");
                resetBuffer();
                break;

            case ';':   // BLE monitor apps don't let you add '\r' to a string,
            // so ';' ends a command

            case '\r':  //CTRL('M') carriage return (or "Enter" key)
                // raw input only sends "return" for the keypress
                // line is complete
                println();     // Echo newline too.
                bufferReady = true;
                break;

            case '\n':  //CTRL('J') linefeed
                // ignore newline as 'raw' terminals may not send it.
                // Serial Monitor sends a "\r\n" pair by default
                break;

            default:
                // Otherwise, echo the character and append it to the buffer
                linebuffer[inptr++] = c;
                write(c);
                if (inptr >= BUFSIZE-1) {
                    bufferReady = true; // flush to avoid overflow
                }
                break;
        }
    } while (moreData && !bufferReady);

    return bufferReady;
}

//////////////////////////////////////////////////////////////////////////////
int SimpleSerialShell::execute(const char commandString[])
{
    // overwrites anything in linebuffer; hope you don't need it!
    strncpy(linebuffer, commandString, BUFSIZE);
    return execute();
}

int SimpleSerialShell::split(char *buffer, char **argv, int max_argv)
{
    char *p, *start_of_word;
    int c;
    enum states { REGULAR, IN_WORD, IN_STRING } state = REGULAR;
    int argc = 0;

    for (p = buffer; argc < max_argv && *p != '\0'; p++) {
        c = (unsigned char) *p;
        switch (state) {
        case REGULAR:
            if (isspace(c)) {
                continue;
            }

            if (c == '"') {
                state = IN_STRING;
                start_of_word = p + 1;
                continue;
            }
            state = IN_WORD;
            start_of_word = p;
            continue;

        case IN_STRING:
            if (c == '"') {
                *p = 0;
                argv[argc++] = start_of_word;
                state = REGULAR;
            }
            continue;

        case IN_WORD:
            if (isspace(c)) {
                *p = 0;
                argv[argc++] = start_of_word;
                state = REGULAR;
            }
            continue;
        }
    }

    if (state != REGULAR && argc < max_argv)
        argv[argc++] = start_of_word;

    return argc;

}
//////////////////////////////////////////////////////////////////////////////
int SimpleSerialShell::execute(void)
{
    char * argv[MAXARGS] = {0};
    int argc = 0;
    char *raw_argv[MAXARGS] = {0};
    int raw_argc = 0;
    char *ptr;
    float f;
    SdFile redir;
    Stream *consoleSave = NULL;
    boolean redirOk = false;
    boolean append;
    char **matches = NULL;
    int    n_matches = 0;
    char * catName;
    Command *aCmd = NULL;
    char **floats = NULL;
    int    i, j, nfloats = 0;
    char  *anArg;

    linebuffer[BUFSIZE - 1] = '\0'; // play it safe

    raw_argc = split(linebuffer, raw_argv, MAXARGS);

    if (raw_argc == 0)
    {
        // empty line; no arguments found.
        println(F("OK"));
        resetBuffer();
        return EXIT_SUCCESS;
    }
    argv[argc++] = raw_argv[0];

    m_lastErrNo = 0;
    for ( aCmd = firstCommand; aCmd != NULL; aCmd = aCmd->next) {
        if (aCmd->compareName(argv[0]) == 0) {
            break;
        }
    }

    if (aCmd == NULL && fallback && (m_lastErrNo = fallback(argc, argv)) == 0) {
        resetBuffer();
        return m_lastErrNo;
    }
    
    if (aCmd == NULL) {
        print(F("\""));
        print(argv[0]);
        print(F("\": "));

        return report(F("command not found"), -1);
    }

    // TODO: handle quoted args, glob (fnmatch), variable expansion, redirection
    for (j = 1 ; j < raw_argc && argc < MAXARGS ; j++)
    {
        anArg = raw_argv[j];
        if (aCmd -> glob && (strchr(anArg, '*') || strchr(anArg, '?'))) {
            matches = glob(sd, anArg, &n_matches);
            for (i = 0 ; i < n_matches && argc < MAXARGS ; i++) {
                argv[argc++] = matches[i];
            }    
        }
        else if (anArg[0] == '>' && j < raw_argc - 1) {
            append = strstr(anArg, ">>") ? true : false;
            catName = raw_argv[++ j];
            redirOk = redir.open(catName, append ? O_WRITE | O_CREAT | O_AT_END : O_WRITE | O_CREAT);
        }
        else if (anArg[0] == '_' && floatExpand) {
            f = floatExpand(anArg + 1);
            if (f == NAN) {
                argv[argc++] = NULL; // or empty string?
            }
            else {
                floats = (char **) realloc(floats, sizeof(char *)*(nfloats + 1));
                floats[nfloats] = (char *) malloc(sizeof(char) * 12);
                snprintf(floats[nfloats], 10, "%f", f);
                argv[argc++] = floats[nfloats];
                nfloats ++;
            }    
        }
        else if (anArg[0] == '$' && stringExpand) {
            if ((ptr = stringExpand(anArg + 1)) != NULL)
                argv[argc++] = ptr;
            else
                argv[argc++] = NULL; // or empty string??
        }
        else if (anArg) {
            argv[argc++] = anArg;
        } 
        else {
            println("uh oh");
            // uh oh?
        }
    }
    // no more arguments - set redirect if set and execute
    if (redirOk && consoleChange) {
        consoleSave = consoleChange(NULL, &redir);
    }
    m_lastErrNo = aCmd->execute(argc, argv);
    resetBuffer();
    
    // restore redirect
    if (redirOk && consoleChange) {
        consoleChange(consoleSave, NULL);
        redir.close();
    }

    // cleanup memory
    for(i = 0 ; i < nfloats ; i++)
        free(floats[i]);

    if (nfloats)
        free(floats);

    for (i = 0 ; i < n_matches ; i++)
        free(matches[i]);

    if (n_matches)
        free(matches);

    return m_lastErrNo;
  
}

//////////////////////////////////////////////////////////////////////////////
int SimpleSerialShell::lastErrNo(void)
{
    return m_lastErrNo;
}

//////////////////////////////////////////////////////////////////////////////
int SimpleSerialShell::report(const __FlashStringHelper * constMsg, int errorCode)
{
    if (errorCode != EXIT_SUCCESS)
    {
        String message(constMsg);
        print(errorCode);
        if (message[0] != '\0') {
            print(F(": "));
            println(message);
        }
    }
    resetBuffer();
    m_lastErrNo = errorCode;
    return errorCode;
}
//////////////////////////////////////////////////////////////////////////////
void SimpleSerialShell::resetBuffer(void)
{
    memset(linebuffer, 0, sizeof(linebuffer));
    inptr = 0;
}

//////////////////////////////////////////////////////////////////////////////
// SimpleSerialShell::printHelp() is a static method.
// printHelp() can access the linked list of commands.
//
int SimpleSerialShell::printHelp(int argc, char **argv)
{
    shell.println(F("Commands available are:"));
    auto aCmd = firstCommand;  // first in list of commands.
    while (aCmd)
    {
        shell.print(F("  "));
        shell.print(aCmd->name);
        shell.print(F("  "));
        shell.println(aCmd->usage);

        aCmd = aCmd->next;
    }
    return 0;	// OK or "no errors"
}

///////////////////////////////////////////////////////////////
// i/o stream indirection/delegation
//
size_t SimpleSerialShell::write(uint8_t aByte)
{
    return shellConnection ?
           shellConnection->write(aByte)
           : 0;
}

int SimpleSerialShell::available()
{
    return shellConnection ? shellConnection->available() : 0;
}

int SimpleSerialShell::read()
{
    return shellConnection ? shellConnection->read() : 0;
}

int SimpleSerialShell::peek()
{
    return shellConnection ? shellConnection->peek() : 0;
}

void SimpleSerialShell::flush()
{
    if (shellConnection)
        shellConnection->flush();
}
