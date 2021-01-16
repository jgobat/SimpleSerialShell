
#ifndef SIMPLE_SERIAL_SHELL_H
#define SIMPLE_SERIAL_SHELL_H

////////////////////////////////////////////////////////////////////////////////
/*!
 *  @file SimpleSerialShell.h
 *
 *  @section dependencies Dependencies
 *
 *  Depends on Stream.  The shell is an instance of Stream so anthing that
 *  works with a Stream should also work with the shell.
 *
 *  @section author Phil Jansen
 */
class SimpleSerialShell : public Stream {
    public:
        SimpleSerialShell(void);

        // Unix-style (from 1970!)
        // functions must have a signature like: "int hello(int argc, char ** argv)"
        typedef int (*CommandFunction)(int, char ** );
        //
        //void addCommand(const char * name, CommandFunction f);
        void addCommand(const __FlashStringHelper * name, CommandFunction f, boolean g, const __FlashStringHelper *usage);

        void attach(Stream & shellSource);
        void addFallback(int (*fb)(int argc, char **argv));
        void addStrings(char *(*stringVar)(char *arg));
        void addFloats(float (*floatVar)(char *arg));
        void addRedirector(Stream *(*redirect)(Stream *c, SdFile *f));
        void addSD(SdFat *s);

        // check for a complete command and run it if available
        // non blocking
        bool executeIfInput(void);  // returns true when command attempted
        int lastErrNo(void);

        char **glob(SdFat *, char *, int *);
        int execute( const char aCommandString[]);  // shell.execute("echo hello world");
        int split(char *, char **, int);
        static int printHelp(int argc, char **argv);

        void resetBuffer(void);

        // this shell delegates communication to/from the attached stream
        // (which sent the command)
        // Note changing streams may intermix serial data
        //
        virtual size_t write(uint8_t);
        virtual int available();
        virtual int read();
        virtual int peek();
        virtual void flush(); // esp32 needs an implementation

    private:
        Stream * shellConnection;
        int m_lastErrNo;
        int execute(void);
        // int execute(int argc, char** argv);

        bool prepInput(void);

        int report(const __FlashStringHelper * message, int errorCode);
        static const char BUFSIZE = 88;
        static const char MAXARGS = 10;
        char linebuffer[BUFSIZE];
        int inptr;
        int (*fallback)(int argc, char **argv);
        char *(*stringExpand)(char *name);
        float (*floatExpand)(char *name);
        SdFat *sd;
        Stream *(*consoleChange)(Stream *c, SdFile *f);
        class Command;
        static Command * firstCommand;
};

////////////////////////////////////////////////////////////////////////////////
extern SimpleSerialShell shell;

//example commands which would be easy to add to the shell:
//extern int helloWorld(int argc, char **argv);
//extern int echo(int argc, char **argv);

#endif /* SIMPLE_SERIAL_SHELL_H */
