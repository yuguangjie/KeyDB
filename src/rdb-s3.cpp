extern "C" {
#include "rio.h"
}
#include "server.h"
#include <unistd.h>
#include <sys/wait.h>

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
int rdbSaveS3(char *s3bucket, rdbSaveInfo *rsi)
{
    int status = EXIT_FAILURE;
    int fd[2];
    if (pipe(fd) != 0)
        return C_ERR;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fd[0]);
        close(fd[1]);
        return C_ERR;
    }

    if (pid == 0)
    {
        // child process
        dup2(fd[0], STDIN_FILENO);
        close(fd[1]);
        close(fd[0]);
        execlp("aws", "aws", "s3", "cp", "-", s3bucket, nullptr);
        exit(EXIT_FAILURE);
    }
    else
    {
        close(fd[0]);
        FILE *fp = fdopen(fd[1], "w");
        if (fp == NULL)
        {
            close (fd[1]);
            return C_ERR;
        }

        if (rdbSaveFp(fp, rsi) != C_OK)
        {
            fclose(fp);
            return C_ERR;
        }
        fclose(fp);
        waitpid(pid, &status, 0);
    }
    
    if (status != EXIT_SUCCESS)
        serverLog(LL_WARNING, "Failed to save DB to AWS S3");
    else
        serverLog(LL_NOTICE,"DB saved on AWS S3");
        
    return (status == EXIT_SUCCESS) ? C_OK : C_ERR;
}


int rdbLoadS3Core(int fd, rdbSaveInfo *rsi) 
{
    FILE *fp;
    rio rdb;
    int retval;

    if ((fp = fdopen(fd, "rb")) == NULL) return C_ERR;
    startLoading(fp);
    rioInitWithFile(&rdb,fp);
    retval = rdbLoadRio(&rdb,rsi,0);
    fclose(fp);
    stopLoading();
    return retval;
}

int rdbLoadS3(char *s3bucket, rdbSaveInfo *rsi)
{
    int status = EXIT_FAILURE;
    int fd[2];
    if (pipe(fd) != 0)
        return C_ERR;

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fd[0]);
        close(fd[1]);
        return C_ERR;
    }

    if (pid == 0)
    {
        // child process
        dup2(fd[1], STDOUT_FILENO);
        close(fd[1]);
        close(fd[0]);
        execlp("aws", "aws", "s3", "cp", s3bucket, "-", nullptr);
        exit(EXIT_FAILURE);
    }
    else
    {
        close(fd[1]);
        if (rdbLoadS3Core(fd[0], rsi) != C_OK)
        {
            close(fd[0]);
            return C_ERR;
        }
        close(fd[0]);
        waitpid(pid, &status, 0);
    }
    
    if (status != EXIT_SUCCESS)
        serverLog(LL_WARNING, "Failed to load DB from AWS S3");
    else
        serverLog(LL_NOTICE,"DB loaded from AWS S3");
        
    return (status == EXIT_SUCCESS) ? C_OK : C_ERR;
}
