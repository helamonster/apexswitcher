# Code Style and Formatting Rules

Always follow these rules in this project.

## Braces

- Open curly braces go on their own line (Allman style).
- Always use `{}` for code blocks even if they contain only a single statement.
- Never use `else if`. The `else` keyword must always be followed by a `{` on a new line. If further branching is needed, put an `if` inside the `else` block.

Example:
```c
if ( a )
{
    ...
}
else
{
    if ( b )
    {
        ...
    }
    else
    {
        ...
    }
}
```

## Spacing

- Put a space between any variable/operand and any operator, including unary operators.
  Examples: `i ++`, `i --`, `++ i`, `-- i`, `! x`, `* ptr`, `& var`
- Put spaces around all binary operators: `a + b`, `a == b`, `a && b`, `a | b`, etc.
- Put a space between the function name and `(`.
- Put a space after every `(` and before every `)`, in all contexts: function calls,
  control structures (`if`, `while`, `for`, `switch`), casts, and grouped sub-expressions.
- Put spaces around each `,` between arguments.

Examples:
```
foo ( arg1 , arg2 , arg3 )
if ( a == b )
( int ) x
* ( long * ) ptr
& var
i ++
! flag
```

## Blank Lines

- Put a blank line as the **first line** inside every function body, `switch` statement, and similar block.
- Put a blank line **after the last `case` line** (between the case label(s) and the first statement inside that case).
- Put a blank line **before every `break`** statement inside a `switch`.
- Put a blank line **after every `break`** statement inside a `switch`.

## Switch Statements

- The `break` statement is indented at the **same level as `case`** — one level less than the code inside the case body.
- Always put a blank line after the last `case` line before the case body (see Blank Lines above).
- Multiple `case` labels on one line, or stacked on consecutive lines, is fine when there are many together.

Example:
```c
switch ( x )
{

    case XK_Left:

        code;

    break;

    case XK_A: case XK_B: case XK_C:
    case XK_D: case XK_E:

        other_code;

    break;

}

## Comments

- Give every function at least a one-line description comment directly above it.
- Put a comment above every logical step inside a function that spans multiple lines.
- Put an inline comment on every variable at the point where it is defined.

## Separators Between Functions

- Between every pair of consecutive function definitions, put exactly:
  - 2 blank lines
  - one line of 80 `/` characters
  - 2 blank lines

- Before `main()` specifically, put 3 lines of 80 `/` characters (with the same surrounding blank lines).

## Variable Alignment

- When a group of related variables are declared or assigned together, align the `=` signs (and values) so the values line up vertically.

## File Layout

- After the `#include` block, place global variable declarations, then function definitions in a logical order, with the separators described above between each function.
