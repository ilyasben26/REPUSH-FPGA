----------------------------------------------------------------------------------
-- Company: 
-- Engineer: 
-- 
-- Create Date:    22:55:51 02/18/2026 
-- Design Name: 
-- Module Name:    uart-state-machine - Behavioral 
-- Project Name: 
-- Target Devices: 
-- Tool versions: 
-- Description: 
--
-- Dependencies: 
--
-- Revision: 
-- Revision 0.01 - File Created
-- Additional Comments: 
--
----------------------------------------------------------------------------------
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

-- Uncomment the following library declaration if using
-- arithmetic functions with Signed or Unsigned values
--use IEEE.NUMERIC_STD.ALL;

-- Uncomment the following library declaration if instantiating
-- any Xilinx primitives in this code.
--library UNISIM;
--use UNISIM.VComponents.all;

-- code taken from https://www.youtube.com/watch?v=es0mXz-CCqI

entity uart_receiver is
    generic (
        c_clkfreq : integer := 27_000_000;
        c_baudrate : integer := 115_200
    );
    port (
        clk : in std_logic;
        rx_i : in std_logic;
        read_ready : in std_logic;  -- Handshake: host ready to accept command
        payload_data : out std_logic_vector(63 downto 0);  -- 64-bit command payload
        payload_valid : out std_logic;  -- Handshake: payload ready flag
        leds_o : out std_logic_vector (7 downto 0) -- Basys2 only has 8 LEDs (debug)
    );
end uart_receiver;

architecture Behavioral of uart_receiver is

    constant c_bittimerlim : integer := c_clkfreq / c_baudrate; -- 27_000_000 / 115_200 = 434
    type states is (S_IDLE, S_START, S_DATA, S_STOP);
    signal state : states := S_IDLE;

    signal bittimer : integer range 0 to c_bittimerlim := 0;
    signal bitcntr : integer range 0 to 7 := 0;
    signal shreg : std_logic_vector(7 downto 0) := (others => '0');
    signal rx_done_tick : std_logic := '0';
    signal leds : std_logic_vector(7 downto 0) := (others => '0');
    signal reset_counter : integer range 0 to 255 := 0;
    signal reset : std_logic := '1';
    
    -- Payload accumulation signals
    signal payload_buffer : std_logic_vector(63 downto 0) := (others => '0');
    signal byte_count : integer range 0 to 7 := 0;
    signal payload_ready : std_logic := '0';
    signal payload_accepted : std_logic := '0';

begin
    -- Power-on reset generator
    process(clk)
    begin
        if rising_edge(clk) then
            if reset_counter < 255 then
                reset_counter <= reset_counter + 1;
                reset <= '1';
            else
                reset <= '0';
            end if;
        end if;
    end process;

    -- Main state machine
    process(clk)
    begin
        if rising_edge(clk) then
            if reset = '1' then
                state <= S_IDLE;
                leds <= (others => '0');
                bittimer <= 0;
                bitcntr <= 0;
                rx_done_tick <= '0';
            else
                case state is
                    when S_IDLE =>
                        rx_done_tick <= '0';
                        if rx_i = '0' then
                            state <= S_START;
                            bittimer <= 0;
                        end if;
                    when S_START =>
                        if bittimer = c_bittimerlim/2 - 1 then
                            state <= S_DATA;
                            bittimer <= 0;
                        else
                            bittimer <= bittimer + 1;
                        end if;
                    when S_DATA =>
                        if bittimer = c_bittimerlim - 1 then
                            shreg <= rx_i & shreg(7 downto 1);
                            if bitcntr = 7 then
                                state <= S_STOP;
                                bitcntr <= 0;
                            else
                                bitcntr <= bitcntr + 1;
                            end if;
                            bittimer <= 0;
                        else
                            bittimer <= bittimer + 1;
                        end if;
                    when S_STOP =>
                        if bittimer = c_bittimerlim - 1 then
                            state <= S_IDLE;
                            rx_done_tick <= '1';
                            leds(7 downto 0) <= shreg;
                            
                            -- Clear payload_ready if it was just accepted
                            if payload_accepted = '1' then
                                payload_ready <= '0';
                            end if;
                            
                            -- Accumulate bytes into 64-bit payload
                            if byte_count < 7 then
                                payload_buffer(63 - byte_count*8 downto 56 - byte_count*8) <= shreg;
                                byte_count <= byte_count + 1;
                            else
                                -- Last byte received, complete payload
                                payload_buffer(7 downto 0) <= shreg;
                                byte_count <= 0;
                                payload_ready <= '1';
                            end if;
                        else
                            bittimer <= bittimer + 1;
                        end if;
                end case;
            end if;
        end if;
    end process;

    -- Handshake logic: payload valid only when ready and host acknowledges
    payload_valid <= payload_ready and read_ready;
    payload_data <= payload_buffer;
    
    -- Track when payload is accepted so we can clear the ready flag next cycle
    process(clk)
    begin
        if rising_edge(clk) then
            payload_accepted <= payload_ready and read_ready;
        end if;
    end process;
    
    leds_o <= leds;

end Behavioral;


