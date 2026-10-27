#include "solar_os_irc_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    solar_os_irc_message_t message;
    char line[] = "@ignored=value :Alice!user@host PRIVMSG #Room :hello : world";
    assert(solar_os_irc_parse(line, &message));
    assert(strcmp(message.prefix, "Alice!user@host") == 0);
    assert(strcmp(message.command, "PRIVMSG") == 0 && message.count == 2);
    assert(strcmp(message.params[0], "#Room") == 0);
    assert(strcmp(message.params[1], "hello : world") == 0);
    char ping[] = "PING :token with spaces";
    assert(solar_os_irc_parse(ping, &message) && message.count == 1);
    assert(strcmp(message.params[0], "token with spaces") == 0);
    char empty[] = "NOTICE me :";
    assert(solar_os_irc_parse(empty, &message) && message.count == 2 && !message.params[1][0]);
    char broken[] = ":bad-prefix";
    assert(!solar_os_irc_parse(broken, &message));
    char many[] = "CMD a b c d e f g h i j k l m n o p";
    assert(!solar_os_irc_parse(many, &message));
    char folded[64];
    solar_os_irc_casefold("AZ[]\\^", folded, sizeof(folded), SOLAR_OS_IRC_CASE_RFC1459);
    assert(strcmp(folded, "az{}|~") == 0);
    solar_os_irc_casefold("AZ[]\\^", folded, sizeof(folded), SOLAR_OS_IRC_CASE_STRICT_RFC1459);
    assert(strcmp(folded, "az{}|^") == 0);
    solar_os_irc_casefold("AZ[]\\^", folded, sizeof(folded), SOLAR_OS_IRC_CASE_ASCII);
    assert(strcmp(folded, "az[]\\^") == 0);
    char unsafe[] = "\033[2Jhi\00304,12red\002\r\n";
    solar_os_irc_clean_text(unsafe);
    assert(strcmp(unsafe, "[2Jhired") == 0);
    assert(solar_os_irc_target_valid("#room", 64));
    assert(!solar_os_irc_target_valid("#room,#other", 64));
    assert(!solar_os_irc_target_valid("nick\r\nJOIN #bad", 64));
    assert(!solar_os_irc_target_valid(":nick", 64));
    assert(!solar_os_irc_atom_valid("", 32));
    assert(solar_os_irc_nick_valid("nils-sol_2", 32));
    assert(solar_os_irc_nick_valid("[nils]", 32));
    assert(!solar_os_irc_nick_valid("nils@sol", 32));
    assert(!solar_os_irc_nick_valid("2nils", 32));
    assert(!solar_os_irc_nick_valid("-nils", 32));
    assert(!solar_os_irc_nick_valid("nils!user", 32));
    assert(!solar_os_irc_nick_valid("nils\r\nOPER", 32));
    assert(!solar_os_irc_nick_valid("", 32));
    puts("IRC protocol tests: ok");
    return 0;
}
