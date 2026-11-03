"""Paced commands for the SolarOS Python REPL over Telnet or a port shell."""
import time


class PythonRepl:
    def __init__(self, console, record, line_bytes=72):
        # console is a getter because a harness may switch between transports.
        self.console = console
        self.record = record
        self.line_bytes = line_bytes

    def _line(self, text):
        assert len(text.encode()) <= self.line_bytes
        console = self.console()
        data = (text + '\r\n').encode()
        for offset in range(0, len(data), 16):
            console.socket.sendall(data[offset:offset + 16])
            time.sleep(.02)
        output = ''
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            output += console.read(.15)
            if '>>> ' in output:
                self.record(output)
                if 'Traceback' in output or 'Guru Meditation' in output:
                    raise AssertionError(output)
                return output
        self.record(output)
        raise TimeoutError('Python prompt missing: ' + output[-1000:])

    def command(self, text):
        # The editor bounds input to visible columns, even though its backing
        # buffer holds 192 bytes. Longer commands are assembled in this owned
        # interpreter namespace and then executed with normal REPL semantics.
        if len(text.encode()) <= self.line_bytes:
            return self._line(text)
        self._line("_hil_line=''")
        while text:
            count = min(24, len(text))
            while len(('_hil_line+=' + repr(text[:count])).encode()) > self.line_bytes:
                count -= 1
            assert count > 0
            self._line('_hil_line+=' + repr(text[:count]))
            text = text[count:]
        return self._line('exec(_hil_line)')
