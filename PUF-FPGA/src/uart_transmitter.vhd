----------------------------------------------------------------------------------
-- Company: 
-- Engineer: 
-- 
-- Create Date:    23:20:59 02/21/2026 
-- Design Name: 
-- Module Name:    UARTTransmitterStateMachine - Behavioral 
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
use IEEE.STD_LOGIC_ARITH.ALL;
use IEEE.STD_LOGIC_UNSIGNED.ALL;

-- Uncomment the following library declaration if using
-- arithmetic functions with Signed or Unsigned values
--use IEEE.NUMERIC_STD.ALL;

-- Uncomment the following library declaration if instantiating
-- any Xilinx primitives in this code.
--library UNISIM;
--use UNISIM.VComponents.all;

-- code originally taken from https://www.youtube.com/watch?v=xMEkWCPUCzM

entity uart_transmitter is
  generic (
        c_clkfreq : INTEGER := 27_000_000;
        c_baudrate : INTEGER := 115_200;
        c_stopbit : INTEGER := 1
    );
    port (
        clk : in STD_LOGIC;
        start_i : in STD_LOGIC; -- single-cycle start pulse
        data_i : in STD_LOGIC_VECTOR(7 downto 0); -- data to be transmitted
        tx_o : out STD_LOGIC;
        tx_done_tick : out STD_LOGIC;
        busy_o : out STD_LOGIC;
        write_ready : out STD_LOGIC  -- Ready to accept next byte
    );
end uart_transmitter;

architecture Behavioral of uart_transmitter is

    -- number of clock cycles per bit
    constant c_bittimerlim : INTEGER := c_clkfreq / c_baudrate;

    -- number of clock cycles for the stop bit(s)
    constant c_stopbitlim : INTEGER := (c_clkfreq / c_baudrate) * c_stopbit;

    type states is (S_IDLE, S_START, S_DATA, S_STOP);
    signal state : states := S_IDLE;

    -- timer for bit duration control
    signal bittimer : INTEGER range 0 to c_stopbitlim := 0;

    -- bit counter to keep track of transmitted bits
    signal bitcntr : INTEGER range 0 to 7 := 0;

    -- shift register to hold the data to be transmitted
    signal shreg : STD_LOGIC_VECTOR(7 downto 0) := (others => '0');

begin
    -- UART Transmission process
    P_UART_TX : process (clk)
    begin
        if (rising_edge(clk)) then

            case state is
                -- Idle state: waiting for transmission to start
                when S_IDLE =>
                    tx_o           <= '1'; -- keep tx line high (idle state)
                    tx_done_tick   <= '0'; -- reset done signal
                    bitcntr        <= 0; -- reset bit counter
                    bittimer       <= 0; -- reset timer
                    
                    if (start_i = '1') then -- if start signal is received
                        state  <= S_START; -- move to start state
                        tx_o   <= '0'; -- start bit (pull tx low)
                        shreg  <= data_i; -- load input data into shift register
                    end if;
                
                -- Start bit transmission
                when S_START =>
                    if (bittimer = c_bittimerlim - 1) then -- if bit duration is complete
                        state                <= S_DATA; -- move to data state
                        tx_o                 <= shreg(0); -- transmit LSB first
                        shreg(7)             <= shreg(0); -- shift register update
                        shreg(6 downto 0)    <= shreg(7 downto 1);
                        bittimer             <= 0; -- reset timer
                    else
                        bittimer             <= bittimer + 1; -- increment timer
                    end if;

                -- Data transmission (8 bits)
                when S_DATA =>
                    if (bitcntr = 7) then -- if all bits are transmitted (first LSB bit was handled in S_START, hence why only 7)
                        if (bittimer = c_bittimerlim - 1) then
                            bitcntr  <= 0; -- reset bit counter
                            state    <= S_STOP; -- move to stop state
                            tx_o     <= '1'; -- send stop bit
                            bittimer <= 0; -- reset timer
                        else
                            bittimer <= bittimer + 1;
                        end if;            
                    else -- if there are more bits to send
                        if (bittimer = c_bittimerlim - 1) then
                            shreg(7)           <= shreg(0); -- shift register update
                            shreg(6 downto 0)  <= shreg(7 downto 1);     
                            tx_o               <= shreg(0); -- transmit next bit
                            bitcntr            <= bitcntr + 1; -- increment bit counter
                            bittimer           <= 0; -- reset timer
                        else
                            bittimer           <= bittimer + 1;                    
                        end if;
                    end if;

                -- Stop bit transmission
                when S_STOP =>
                    if (bittimer = c_stopbitlim - 1) then -- if stop bit duration is complete
                        state         <= S_IDLE; -- go back to idle state
                        tx_done_tick  <= '1'; -- indicate transmission complete
                        bittimer      <= 0; -- reset timer
                    else
                        bittimer      <= bittimer + 1;                
                    end if;        
            end case;
        end if;
    end process;

    -- busy flag
    busy_o <= '1' when (state /= S_IDLE) else '0';
    
    -- write_ready flag: ready to accept new data when idle
    write_ready <= '1' when (state = S_IDLE) else '0';


end Behavioral;

