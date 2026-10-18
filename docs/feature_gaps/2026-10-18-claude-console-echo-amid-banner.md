# A console's reply to a line typed as it starts

CI's macOS job at `7cbcf76` failed `test_qucs_control consolesAreTypedIntoAskedEachTime`. The Python console's reply to `print(6*7)` was this, not "42":

```
Type "help", "copyright", "credits" or "license" for more information.
>>> print(6*7)
42
```

## The cause

The first call starts the Python shell and types its line at once, so the line reaches the terminal while Python is still printing its banner:
- **The terminal echoed it amid the banner,** between the banner's two lines.
- **Python's line editor showed it again after its prompt.**

The reply is what follows the line's echo, and the first echo found was the terminal's. Everything after it came back: the banner's second line, the line again, and 42. When the shell was already running, or the line arrived after the banner, the two echoes were next to each other and the rule worked. That is why the test passed on the run before and here.

## Now

The echo after a prompt is taken first (`qucs_s::control::consoleReply`). The terminal's own echo is taken only when there is none after a prompt.

## Test and break

- **test_qucs_control `consolesAreTypedIntoAskedEachTime`:** CI's interleaving as a case of its own, which gives "42". The earlier cases give what they did.
- **The first echo taken again:** caught.
