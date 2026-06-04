library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

-- This module interfaces the UART receiver/transmitter with the PUF controller
-- It handles the protocol translation and buffering

entity uart_puf_interface is
    Port ( 
        clk : in STD_LOGIC;
        
        -- From UART Receiver (command input)
        payload_data : in STD_LOGIC_VECTOR(63 downto 0);
        payload_valid : in STD_LOGIC;
        
        -- To/From PUF Controller
        puf_payload : out STD_LOGIC_VECTOR(63 downto 0);
        puf_read_valid : out STD_LOGIC;
        puf_read_ready : in STD_LOGIC;
        puf_write_valid : in STD_LOGIC;
        puf_write_ready : out STD_LOGIC;
        puf_response : in STD_LOGIC_VECTOR(63 downto 0);
        
        -- To UART Transmitter (response output)
        tx_start : out STD_LOGIC;
        tx_data : out STD_LOGIC_VECTOR(7 downto 0);
        tx_busy : in STD_LOGIC;
        tx_write_ready : in STD_LOGIC
    );
end uart_puf_interface;

architecture Behavioral of uart_puf_interface is

    type state_type is (S_IDLE, S_FWD_CMD, S_WAIT_RESP, 
                        S_TX_BYTE, S_TX_ACK, S_TX_WAIT);
    signal state : state_type := S_IDLE;
    
    -- Response buffer (64-bit PUF response)
    signal response_buffer : STD_LOGIC_VECTOR(63 downto 0) := (others => '0');
    signal byte_index : integer range 0 to 7 := 0;
    signal tx_start_pulse : STD_LOGIC := '0';
    signal is_req_command : STD_LOGIC := '0';  -- Track if command requires response

begin

    -- Main state machine
    process(clk)
    begin
        if rising_edge(clk) then
            tx_start_pulse <= '0';  -- Default: no transmission start
            
            case state is
                -- Idle: waiting for command from UART
                when S_IDLE =>
                    puf_read_valid <= '0';
                    puf_write_ready <= '1';
                    is_req_command <= '0';
                    
                    -- UART has a command to forward
                    if payload_valid = '1' and puf_read_ready = '1' then
                        puf_payload <= payload_data;
                        puf_read_valid <= '1';
                        -- Check if this is a PUF_REQ command (0x1 = request measurement)
                        if payload_data(63 downto 60) = "0001" then
                            is_req_command <= '1';
                        end if;
                        state <= S_FWD_CMD;
                    end if;
                
                -- Forward command pulse to PUF controller
                when S_FWD_CMD =>
                    puf_read_valid <= '0';  -- One-cycle pulse
                    puf_write_ready <= '1';
                    -- Only wait for response if this is a PUF_REQ command
                    if is_req_command = '1' then
                        state <= S_WAIT_RESP;
                    else
                        state <= S_IDLE;  -- Non-req commands don't generate responses
                    end if;
                
                -- Wait for PUF response (only for PUF_REQ commands)
                when S_WAIT_RESP =>
                    puf_write_ready <= '1';
                    if puf_write_valid = '1' then
                        response_buffer <= puf_response;
                        byte_index <= 0;
                        state <= S_TX_BYTE;
                    end if;
                
                -- Transmit byte based on index (MSB first)
                when S_TX_BYTE =>
                    puf_write_ready <= '0';
                    -- Select byte dynamically based on index (7 down to 0)
                    tx_data <= response_buffer((7 - byte_index) * 8 + 7 downto (7 - byte_index) * 8);
                    tx_start_pulse <= '1';
                    state <= S_TX_ACK;
                
                when S_TX_ACK =>
                    -- Wait for transmitter to accept (write_ready goes low)
                    if tx_write_ready = '0' then
                        state <= S_TX_WAIT;
                    end if;
                
                when S_TX_WAIT =>
                    -- Wait for transmitter to finish (write_ready goes high)
                    if tx_write_ready = '1' then
                        if byte_index = 7 then
                            state <= S_IDLE;
                        else
                            byte_index <= byte_index + 1;
                            state <= S_TX_BYTE;
                        end if;
                    end if;
                
                when others =>
                    state <= S_IDLE;
            end case;
        end if;
    end process;
    
    tx_start <= tx_start_pulse;

end Behavioral;
