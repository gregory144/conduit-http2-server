#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "h2_test_cmd.h"
#include "h2_test_cmd_parser.h"
#include "h2_test_cmd_scanner.h"

#ifndef YY_TYPEDEF_YY_SCANNER_T
#define YY_TYPEDEF_YY_SCANNER_T
typedef void * yyscan_t;
#endif

int yyparse(struct h2_frame_parser_s * frame_parser, h2_test_cmd_list_t * * test_cmd, yyscan_t scanner);

h2_test_cmd_list_t * h2_test_cmd_list_parse(h2_frame_parser_t * frame_parser, FILE * fp)
{
  yyscan_t scanner;

  if (yylex_init(&scanner)) {
    // couldn't initialize
    return NULL;
  }

  YY_BUFFER_STATE state = yy_create_buffer(fp, YY_BUF_SIZE, scanner);
  yy_switch_to_buffer(state, scanner);

  h2_test_cmd_list_t * test_cmd = NULL;
  if (yyparse(frame_parser, &test_cmd, scanner)) {
    // error parsing
    return NULL;
  }

  yy_delete_buffer(state, scanner);

  yylex_destroy(scanner);

  return test_cmd;
}

h2_test_cmd_list_t * h2_test_cmd_list_append(h2_test_cmd_list_t * test_cmd, h2_test_cmd_t * cmd)
{
  h2_test_cmd_list_t * next = malloc(sizeof(h2_test_cmd_list_t));
  next->cmd = cmd;
  next->next = NULL;

  h2_test_cmd_list_t * curr = test_cmd;
  if (!curr) {
    return next;
  }
  while (curr->next) {
    curr = curr->next;
  }
  curr->next = next;

  return test_cmd;
}
